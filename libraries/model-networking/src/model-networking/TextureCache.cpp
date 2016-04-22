//
//  TextureCache.cpp
//  libraries/model-networking/src
//
//  Created by Andrzej Kapolka on 8/6/13.
//  Copyright 2013 High Fidelity, Inc.
//
//  Distributed under the Apache License, Version 2.0.
//  See the accompanying file LICENSE or http://www.apache.org/licenses/LICENSE-2.0.html
//

#include "TextureCache.h"

#include <mutex>

#include <QNetworkReply>
#include <QImage>
#include <QPainter>
#include <QRunnable>
#include <QThreadPool>
#include <QImageReader>

#include <glm/glm.hpp>
#include <glm/gtc/random.hpp>

#include <gpu/Batch.h>

#include <shared/NsightHelpers.h>

#include <Finally.h>
#include <PathUtils.h>

#include "ModelNetworkingLogging.h"

// FIXME: Declare this to enable compression
//#define COMPRESS_TEXTURES

TextureCache::TextureCache() {
    const qint64 TEXTURE_DEFAULT_UNUSED_MAX_SIZE = DEFAULT_UNUSED_MAX_SIZE;
    setUnusedResourceCacheSize(TEXTURE_DEFAULT_UNUSED_MAX_SIZE);
    setObjectName("TextureCache");
}

TextureCache::~TextureCache() {
}

// use fixed table of permutations. Could also make ordered list programmatically
// and then shuffle algorithm. For testing, this ensures consistent behavior in each run.
// this list taken from Ken Perlin's Improved Noise reference implementation (orig. in Java) at
// http://mrl.nyu.edu/~perlin/noise/

const int permutation[256] = 
{
    151, 160, 137,  91,  90,  15, 131,  13, 201,  95,  96,  53, 194, 233,   7, 225,
    140,  36, 103,  30,  69, 142,   8,  99,  37, 240,  21,  10,  23, 190,   6, 148,
    247, 120, 234,  75,   0,  26, 197,  62,  94, 252, 219, 203, 117,  35,  11,  32,
     57, 177,  33,  88, 237, 149,  56,  87, 174,  20, 125, 136, 171, 168,  68, 175,
     74, 165,  71, 134, 139,  48,  27, 166,  77, 146, 158, 231,  83, 111, 229, 122,
     60, 211, 133, 230, 220, 105,  92,  41,  55,  46, 245,  40, 244, 102, 143,  54,
     65,  25,  63, 161,   1, 216,  80,  73, 209,  76, 132, 187, 208,  89,  18, 169,
     200, 196, 135, 130, 116, 188, 159, 86, 164, 100, 109, 198, 173, 186,   3,  64,
     52, 217, 226, 250, 124, 123,   5, 202,  38, 147, 118, 126, 255,  82,  85, 212,
    207, 206,  59, 227,  47,  16,  58,  17, 182, 189,  28,  42, 223, 183, 170, 213,
    119, 248, 152,   2,  44, 154, 163,  70, 221, 153, 101, 155, 167,  43, 172,   9,
    129,  22,  39, 253,  19,  98, 108, 110,  79, 113, 224, 232, 178, 185, 112, 104,
    218, 246,  97, 228, 251,  34, 242, 193, 238, 210, 144,  12, 191, 179, 162, 241,
     81,  51, 145, 235, 249,  14, 239, 107,  49, 192, 214,  31, 181, 199, 106, 157,
    184,  84, 204, 176, 115, 121,  50,  45, 127,   4, 150, 254, 138, 236, 205,  93,
    222, 114,  67,  29,  24,  72, 243, 141, 128, 195,  78,  66, 215,  61, 156, 180
};

#define USE_CHRIS_NOISE 1

const gpu::TexturePointer& TextureCache::getPermutationNormalTexture() {
    if (!_permutationNormalTexture) {

        // the first line consists of random permutation offsets
        unsigned char data[256 * 2 * 3];
#if (USE_CHRIS_NOISE==1)
        for (int i = 0; i < 256; i++) {
            data[3*i+0] = permutation[i];
            data[3*i+1] = permutation[i];
            data[3*i+2] = permutation[i];
        }
#else
        for (int i = 0; i < 256 * 3; i++) {
            data[i] = rand() % 256;
        }
#endif

        for (int i = 256 * 3; i < 256 * 3 * 2; i += 3) {
            glm::vec3 randvec = glm::sphericalRand(1.0f);
            data[i] = ((randvec.x + 1.0f) / 2.0f) * 255.0f;
            data[i + 1] = ((randvec.y + 1.0f) / 2.0f) * 255.0f;
            data[i + 2] = ((randvec.z + 1.0f) / 2.0f) * 255.0f;
        }

        _permutationNormalTexture = gpu::TexturePointer(gpu::Texture::create2D(gpu::Element(gpu::VEC3, gpu::NUINT8, gpu::RGB), 256, 2));
        _permutationNormalTexture->assignStoredMip(0, _blueTexture->getTexelFormat(), sizeof(data), data);
    }
    return _permutationNormalTexture;
}

const unsigned char OPAQUE_WHITE[] = { 0xFF, 0xFF, 0xFF, 0xFF };
const unsigned char OPAQUE_GRAY[] = { 0x80, 0x80, 0x80, 0xFF };
const unsigned char OPAQUE_BLUE[] = { 0x80, 0x80, 0xFF, 0xFF };
const unsigned char OPAQUE_BLACK[] = { 0x00, 0x00, 0x00, 0xFF };

const gpu::TexturePointer& TextureCache::getWhiteTexture() {
    if (!_whiteTexture) {
        _whiteTexture = gpu::TexturePointer(gpu::Texture::create2D(gpu::Element::COLOR_RGBA_32, 1, 1));
        _whiteTexture->assignStoredMip(0, _whiteTexture->getTexelFormat(), sizeof(OPAQUE_WHITE), OPAQUE_WHITE);
    }
    return _whiteTexture;
}

const gpu::TexturePointer& TextureCache::getGrayTexture() {
    if (!_grayTexture) {
        _grayTexture = gpu::TexturePointer(gpu::Texture::create2D(gpu::Element::COLOR_RGBA_32, 1, 1));
        _grayTexture->assignStoredMip(0, _whiteTexture->getTexelFormat(), sizeof(OPAQUE_WHITE), OPAQUE_GRAY);
    }
    return _grayTexture;
}

const gpu::TexturePointer& TextureCache::getBlueTexture() {
    if (!_blueTexture) {
        _blueTexture = gpu::TexturePointer(gpu::Texture::create2D(gpu::Element::COLOR_RGBA_32, 1, 1));
        _blueTexture->assignStoredMip(0, _blueTexture->getTexelFormat(), sizeof(OPAQUE_BLUE), OPAQUE_BLUE);
    }
    return _blueTexture;
}

const gpu::TexturePointer& TextureCache::getBlackTexture() {
    if (!_blackTexture) {
        _blackTexture = gpu::TexturePointer(gpu::Texture::create2D(gpu::Element::COLOR_RGBA_32, 1, 1));
        _blackTexture->assignStoredMip(0, _whiteTexture->getTexelFormat(), sizeof(OPAQUE_BLACK), OPAQUE_BLACK);
    }
    return _blackTexture;
}


const gpu::TexturePointer& TextureCache::getNormalFittingTexture() {
    if (!_normalFittingTexture) {
        _normalFittingTexture = getImageTexture(PathUtils::resourcesPath() + "images/normalFittingScale.dds");
    }
    return _normalFittingTexture;
}

const gpu::TexturePointer& TextureCache::getDefaultTexture(TextureType type) {
    switch (type) {
        case DEFAULT_TEXTURE:
        case ALBEDO_TEXTURE:
        case LIGHTMAP_TEXTURE:
            return getGrayTexture();
        case NORMAL_TEXTURE:
        case BUMP_TEXTURE:
            return getBlueTexture();
        case SPECULAR_TEXTURE: // METALLIC_TEXTURE
        case EMISSIVE_TEXTURE:
        case CUBE_TEXTURE:
            return getBlackTexture();
        case ROUGHNESS_TEXTURE:
        case OCCLUSION_TEXTURE:
        case GLOSS_TEXTURE:
        case CUSTOM_TEXTURE:
        default:
            return getWhiteTexture();
    }
};

/// Extra data for creating textures.
class TextureExtra {
public:
    TextureType type;
    const QByteArray& content;
};

NetworkTexturePointer TextureCache::getTexture(const QUrl& url, TextureType type, const QByteArray& content) {
    TextureExtra extra = { type, content };
    return ResourceCache::getResource(url, QUrl(), content.isEmpty(), &extra).staticCast<NetworkTexture>();
}

NetworkTexture::TextureLoaderFunc NetworkTexture::getTextureLoader() const {
    if (_type == CUSTOM_TEXTURE) {
        return _textureLoader;
    }
    return getTextureLoader(_type);
}

NetworkTexture::TextureLoaderFunc NetworkTexture::getTextureLoader(TextureType type) {
    switch (type) {
        case ALBEDO_TEXTURE: {
            return createAlbedoTextureFromImage;
            break;
        }
        case EMISSIVE_TEXTURE: {
            return createEmissiveTextureFromImage;
            break;
        }
        case LIGHTMAP_TEXTURE: {
            return createLightmapTextureFromImage;
            break;
        }
        case CUBE_TEXTURE: {
            return createCubeTextureFromImage;
            break;
        }
        case BUMP_TEXTURE: {
            return createNormalTextureFromBumpImage;
            break;
        }
        case NORMAL_TEXTURE: {
            return createNormalTextureFromNormalImage;
            break;
        }
        case ROUGHNESS_TEXTURE: {
            return createRoughnessTextureFromImage;
            break;
        }
        case GLOSS_TEXTURE: {
            return createRoughnessTextureFromGlossImage;
            break;
        }
        case SPECULAR_TEXTURE: {
            return createMetallicTextureFromImage;
            break;
        }
        case CUSTOM_TEXTURE: {
            Q_ASSERT(false);
            return TextureLoaderFunc();
            break;
        }
        case DEFAULT_TEXTURE:
        default: {
            return create2DTextureFromImage;
            break;
        }
    }
}

/// Returns a texture version of an image file
gpu::TexturePointer TextureCache::getImageTexture(const QString& path, TextureType type) {
    QImage image = QImage(path);
    auto loader = NetworkTexture::getTextureLoader(type);
    return gpu::TexturePointer(loader(image, QUrl::fromLocalFile(path).fileName().toStdString()));
}

QSharedPointer<Resource> TextureCache::createResource(const QUrl& url,
        const QSharedPointer<Resource>& fallback, bool delayLoad, const void* extra) {
    const TextureExtra* textureExtra = static_cast<const TextureExtra*>(extra);
    return QSharedPointer<Resource>(new NetworkTexture(url, textureExtra->type, textureExtra->content),
        &Resource::deleter);
}

NetworkTexture::NetworkTexture(const QUrl& url, TextureType type, const QByteArray& content) :
    Resource(url, !content.isEmpty()),
    _type(type)
{
    _textureSource = std::make_shared<gpu::TextureSource>();

    if (!url.isValid()) {
        _loaded = true;
    }

    std::string theName = url.toString().toStdString();
    // if we have content, load it after we have our self pointer
    if (!content.isEmpty()) {
        _startedLoading = true;
        QMetaObject::invokeMethod(this, "loadContent", Qt::QueuedConnection, Q_ARG(const QByteArray&, content));
    }
}

class ImageReader : public QRunnable {
public:

    ImageReader(const QWeakPointer<Resource>& resource, const QByteArray& data, const QUrl& url = QUrl());

    virtual void run();

private:
    static void listSupportedImageFormats();

    QWeakPointer<Resource> _resource;
    QUrl _url;
    QByteArray _content;
};

void NetworkTexture::downloadFinished(const QByteArray& data) {
    // send the reader off to the thread pool
    QThreadPool::globalInstance()->start(new ImageReader(_self, data, _url));
}

void NetworkTexture::loadContent(const QByteArray& content) {
    QThreadPool::globalInstance()->start(new ImageReader(_self, content, _url));
}

ImageReader::ImageReader(const QWeakPointer<Resource>& resource, const QByteArray& data,
        const QUrl& url) :
    _resource(resource),
    _url(url),
    _content(data)
{
}

void ImageReader::listSupportedImageFormats() {
    static std::once_flag once;
    std::call_once(once, []{
        auto supportedFormats = QImageReader::supportedImageFormats();
        qCDebug(modelnetworking) << "List of supported Image formats:" << supportedFormats.join(", ");
    });
}

void ImageReader::run() {
    PROFILE_RANGE_EX(__FUNCTION__, 0xffff0000, nullptr);
    auto originalPriority = QThread::currentThread()->priority();
    if (originalPriority == QThread::InheritPriority) {
        originalPriority = QThread::NormalPriority;
    }
    QThread::currentThread()->setPriority(QThread::LowPriority);
    Finally restorePriority([originalPriority]{
        QThread::currentThread()->setPriority(originalPriority);
    });

    if (!_resource.data()) {
        qCWarning(modelnetworking) << "Abandoning load of" << _url << "; could not get strong ref";
        return;
    }

    listSupportedImageFormats();

    // Help the QImage loader by extracting the image file format from the url filename ext.
    // Some tga are not created properly without it.
    auto filename = _url.fileName().toStdString();
    auto filenameExtension = filename.substr(filename.find_last_of('.') + 1);
    QImage image = QImage::fromData(_content, filenameExtension.c_str());

    // Note that QImage.format is the pixel format which is different from the "format" of the image file...
    auto imageFormat = image.format();
    int originalWidth = image.width();
    int originalHeight = image.height();

    if (originalWidth == 0 || originalHeight == 0 || imageFormat == QImage::Format_Invalid) {
        if (filenameExtension.empty()) {
            qCDebug(modelnetworking) << "QImage failed to create from content, no file extension:" << _url;
        } else {
            qCDebug(modelnetworking) << "QImage failed to create from content" << _url;
        }
        return;
    }

    gpu::TexturePointer texture = nullptr;
    {
        // Double-check the resource still exists between long operations.
        auto resource = _resource.toStrongRef();
        if (!resource) {
            qCWarning(modelnetworking) << "Abandoning load of" << _url << "; could not get strong ref";
            return;
        }

        auto url = _url.toString().toStdString();

        PROFILE_RANGE_EX(__FUNCTION__"::textureLoader", 0xffffff00, nullptr);
        texture.reset(resource.dynamicCast<NetworkTexture>()->getTextureLoader()(image, url));
    }

    // Ensure the resource has not been deleted
    auto resource = _resource.toStrongRef();
    if (!resource) {
        qCWarning(modelnetworking) << "Abandoning load of" << _url << "; could not get strong ref";
    } else {
        QMetaObject::invokeMethod(resource.data(), "setImage",
            Q_ARG(gpu::TexturePointer, texture),
            Q_ARG(int, originalWidth), Q_ARG(int, originalHeight));
    }
}

void NetworkTexture::setImage(gpu::TexturePointer texture, int originalWidth,
                              int originalHeight) {
    _originalWidth = originalWidth;
    _originalHeight = originalHeight;

    // Passing ownership
    _textureSource->resetTexture(texture);

    if (texture) {
        _width = texture->getWidth();
        _height = texture->getHeight();
        setSize(texture->getStoredSize());
    } else {
        // FIXME: If !gpuTexture, we failed to load!
        _width = _height = 0;
        qWarning() << "Texture did not load";
    }

    finishedLoading(true);

    emit networkTextureCreated(qWeakPointerCast<NetworkTexture, Resource> (_self));
}

const QImage NetworkTexture::process2DImageColor(const QImage& srcImage, bool& validAlpha, bool& alphaAsMask) {
    QImage image = srcImage;
    validAlpha = false;
    alphaAsMask = true;
    const gpu::uint8 OPAQUE_ALPHA = 255;
    const gpu::uint8 TRANSPARENT_ALPHA = 0;
    if (image.hasAlphaChannel()) {
        std::map<gpu::uint8, gpu::uint32> alphaHistogram;

        if (image.format() != QImage::Format_ARGB32) {
            image = image.convertToFormat(QImage::Format_ARGB32);
        }

        // Actual alpha channel? create the histogram
        for (int y = 0; y < image.height(); ++y) {
            const QRgb* data = reinterpret_cast<const QRgb*>(image.constScanLine(y));
            for (int x = 0; x < image.width(); ++x) {
                auto alpha = qAlpha(data[x]);
                alphaHistogram[alpha] ++;
                validAlpha = validAlpha || (alpha != OPAQUE_ALPHA);
            }
        }

        // If alpha was meaningfull refine
        if (validAlpha && (alphaHistogram.size() > 1)) {
            auto totalNumPixels = image.height() * image.width();
            auto numOpaques = alphaHistogram[OPAQUE_ALPHA];
            auto numTransparents = alphaHistogram[TRANSPARENT_ALPHA];
            auto numTranslucents = totalNumPixels - numOpaques - numTransparents;

            alphaAsMask = ((numTranslucents / (double)totalNumPixels) < 0.05);
        }
    }

    if (!validAlpha && image.format() != QImage::Format_RGB888) {
        image = image.convertToFormat(QImage::Format_RGB888);
    }

    return image;
}

void NetworkTexture::defineColorTexelFormats(gpu::Element& formatGPU, gpu::Element& formatMip, 
const QImage& image, bool isLinear, bool doCompress) {

#ifdef COMPRESS_TEXTURES
#else
    doCompress = false;
#endif

    if (image.hasAlphaChannel()) {
        gpu::Semantic gpuSemantic;
        gpu::Semantic mipSemantic;
        if (isLinear) {
            mipSemantic = gpu::SBGRA;
            if (doCompress) {
                gpuSemantic = gpu::COMPRESSED_SRGBA;
            } else {
                gpuSemantic = gpu::SRGBA;
            }
        } else {
            mipSemantic = gpu::BGRA;
            if (doCompress) {
                gpuSemantic = gpu::COMPRESSED_RGBA;
            } else {
                gpuSemantic = gpu::RGBA;
            }
        }
        formatGPU = gpu::Element(gpu::VEC4, gpu::NUINT8, gpuSemantic);
        formatMip = gpu::Element(gpu::VEC4, gpu::NUINT8, mipSemantic);
    } else {
        gpu::Semantic gpuSemantic;
        gpu::Semantic mipSemantic;
        if (isLinear) {
            mipSemantic = gpu::SRGB;
            if (doCompress) {
                gpuSemantic = gpu::COMPRESSED_SRGB;
            } else {
                gpuSemantic = gpu::SRGB;
            }
        } else {
            mipSemantic = gpu::RGB;
            if (doCompress) {
                gpuSemantic = gpu::COMPRESSED_RGB;
            } else {
                gpuSemantic = gpu::RGB;
            }
        }
        formatGPU = gpu::Element(gpu::VEC3, gpu::NUINT8, gpuSemantic);
        formatMip = gpu::Element(gpu::VEC3, gpu::NUINT8, mipSemantic);
    }
}

gpu::Texture* NetworkTexture::process2DTextureColorFromImage(const QImage& srcImage, bool isLinear, bool doCompress, bool generateMips, const gpu::TexturePointer& defaultTexture) {
    bool validAlpha = false;
    bool alphaAsMask = true;
    QImage image = process2DImageColor(srcImage, validAlpha, alphaAsMask);

    gpu::Texture* theTexture = nullptr;

    if ((image.width() > 0) && (image.height() > 0)) {
        gpu::Element formatGPU;
        gpu::Element formatMip;
        defineColorTexelFormats(formatGPU, formatMip, image, isLinear, doCompress);

        theTexture = (gpu::Texture::create2D(formatGPU, image.width(), image.height(), gpu::Sampler(gpu::Sampler::FILTER_MIN_MAG_MIP_LINEAR), defaultTexture));

        auto usage = gpu::Texture::Usage::Builder().withColor();
        if (validAlpha) {
            usage.withAlpha();
            if (alphaAsMask) {
                usage.withAlphaMask();
            }
        }
        theTexture->setUsage(usage.build());

        theTexture->assignStoredMip(0, formatMip, image.byteCount(), image.constBits());

        if (generateMips) {
            theTexture->autoGenerateMips(-1);
            auto levels = theTexture->maxMip();
            uvec2 size(image.width(), image.height());
            for (uint8_t i = 1; i <= levels; ++i) {
                size >>= 1;
                size = glm::max(size, uvec2(1));
                image = image.scaled(size.x, size.y, Qt::IgnoreAspectRatio, Qt::SmoothTransformation);
                theTexture->assignStoredMip(i, formatMip, image.byteCount(), image.constBits());
            }
        }
    }

    return theTexture;
}

gpu::Texture* NetworkTexture::create2DTextureFromImage(const QImage& srcImage, const std::string& srcImageName) {
    return process2DTextureColorFromImage(srcImage, true, false, true, nullptr);
}


gpu::Texture* NetworkTexture::createAlbedoTextureFromImage(const QImage& srcImage, const std::string& srcImageName) {
    return process2DTextureColorFromImage(srcImage, true, true, true, DependencyManager::get<TextureCache>()->getDefaultTexture(ALBEDO_TEXTURE));
}

gpu::Texture* NetworkTexture::createEmissiveTextureFromImage(const QImage& srcImage, const std::string& srcImageName) {
    return process2DTextureColorFromImage(srcImage, true, true, true, DependencyManager::get<TextureCache>()->getDefaultTexture(EMISSIVE_TEXTURE));
}

gpu::Texture* NetworkTexture::createLightmapTextureFromImage(const QImage& srcImage, const std::string& srcImageName) {
    return process2DTextureColorFromImage(srcImage, true, true, true, DependencyManager::get<TextureCache>()->getDefaultTexture(LIGHTMAP_TEXTURE));
}


gpu::Texture* NetworkTexture::createNormalTextureFromNormalImage(const QImage& srcImage, const std::string& srcImageName) {
    QImage image = srcImage;

    if (image.format() != QImage::Format_RGB888) {
        image = image.convertToFormat(QImage::Format_RGB888);
    }

    gpu::Texture* theTexture = nullptr;
    if ((image.width() > 0) && (image.height() > 0)) {
        
        gpu::Element formatGPU = gpu::Element(gpu::VEC3, gpu::NUINT8, gpu::RGB);
        gpu::Element formatMip = gpu::Element(gpu::VEC3, gpu::NUINT8, gpu::RGB);

        theTexture = (gpu::Texture::create2D(formatGPU, image.width(), image.height(), gpu::Sampler(gpu::Sampler::FILTER_MIN_MAG_MIP_LINEAR), DependencyManager::get<TextureCache>()->getDefaultTexture(NORMAL_TEXTURE)));
        theTexture->assignStoredMip(0, formatMip, image.byteCount(), image.constBits());
        theTexture->autoGenerateMips(-1);
    }

    return theTexture;
}

int clampPixelCoordinate(int coordinate, int maxCoordinate) {
    return coordinate - ((int)(coordinate < 0) * coordinate) + ((int)(coordinate > maxCoordinate) * (maxCoordinate - coordinate));
}

const int RGBA_MAX = 255;

// transform -1 - 1 to 0 - 255 (from sobel value to rgb)
double mapComponent(double sobelValue) {
    const double factor = RGBA_MAX / 2.0;
    return (sobelValue + 1.0) * factor;
}

gpu::Texture* NetworkTexture::createNormalTextureFromBumpImage(const QImage& srcImage, const std::string& srcImageName) {
    QImage image = srcImage;
    
    if (image.format() != QImage::Format_RGB888) {
        image = image.convertToFormat(QImage::Format_RGB888);
    }

    // PR 5540 by AlessandroSigna integrated here as a specialized TextureLoader for bumpmaps
    // The conversion is done using the Sobel Filter to calculate the derivatives from the grayscale image
    const double pStrength = 2.0;
    int width = image.width();
    int height = image.height();
    QImage result(width, height, QImage::Format_RGB888);
    
    for (int i = 0; i < width; i++) {
        const int iNextClamped = clampPixelCoordinate(i + 1, width - 1);
        const int iPrevClamped = clampPixelCoordinate(i - 1, width - 1);
    
        for (int j = 0; j < height; j++) {
            const int jNextClamped = clampPixelCoordinate(j + 1, height - 1);
            const int jPrevClamped = clampPixelCoordinate(j - 1, height - 1);
    
            // surrounding pixels
            const QRgb topLeft = image.pixel(iPrevClamped, jPrevClamped);
            const QRgb top = image.pixel(iPrevClamped, j);
            const QRgb topRight = image.pixel(iPrevClamped, jNextClamped);
            const QRgb right = image.pixel(i, jNextClamped);
            const QRgb bottomRight = image.pixel(iNextClamped, jNextClamped);
            const QRgb bottom = image.pixel(iNextClamped, j);
            const QRgb bottomLeft = image.pixel(iNextClamped, jPrevClamped);
            const QRgb left = image.pixel(i, jPrevClamped);
    
            // take their gray intensities
            // since it's a grayscale image, the value of each component RGB is the same
            const double tl = qRed(topLeft);
            const double t = qRed(top);
            const double tr = qRed(topRight);
            const double r = qRed(right);
            const double br = qRed(bottomRight);
            const double b = qRed(bottom);
            const double bl = qRed(bottomLeft);
            const double l = qRed(left);
    
            // apply the sobel filter
            const double dX = (tr + pStrength * r + br) - (tl + pStrength * l + bl);
            const double dY = (bl + pStrength * b + br) - (tl + pStrength * t + tr);
            const double dZ = RGBA_MAX / pStrength;
    
            glm::vec3 v(dX, dY, dZ);
            glm::normalize(v);
    
            // convert to rgb from the value obtained computing the filter
            QRgb qRgbValue = qRgb(mapComponent(v.x), mapComponent(v.y), mapComponent(v.z));
            result.setPixel(i, j, qRgbValue);
        }
    }

    gpu::Texture* theTexture = nullptr;
    if ((image.width() > 0) && (image.height() > 0)) {

        gpu::Element formatGPU = gpu::Element(gpu::VEC3, gpu::NUINT8, gpu::RGB);
        gpu::Element formatMip = gpu::Element(gpu::VEC3, gpu::NUINT8, gpu::RGB);

        theTexture = (gpu::Texture::create2D(formatGPU, image.width(), image.height(), gpu::Sampler(gpu::Sampler::FILTER_MIN_MAG_MIP_LINEAR), DependencyManager::get<TextureCache>()->getDefaultTexture(NORMAL_TEXTURE)));
        theTexture->assignStoredMip(0, formatMip, image.byteCount(), image.constBits());
        theTexture->autoGenerateMips(-1);
    }

    return theTexture;
}

gpu::Texture* NetworkTexture::createRoughnessTextureFromImage(const QImage& srcImage, const std::string& srcImageName) {
    QImage image = srcImage;
    if (!image.hasAlphaChannel()) {
        if (image.format() != QImage::Format_RGB888) {
            image = image.convertToFormat(QImage::Format_RGB888);
        }
    } else {
        if (image.format() != QImage::Format_ARGB32) {
            image = image.convertToFormat(QImage::Format_ARGB32);
        }
    }

    image = image.convertToFormat(QImage::Format_Grayscale8);

    gpu::Texture* theTexture = nullptr;
    if ((image.width() > 0) && (image.height() > 0)) {
#ifdef COMPRESS_TEXTURES
        gpu::Element formatGPU = gpu::Element(gpu::SCALAR, gpu::NUINT8, gpu::COMPRESSED_R);
#else
        gpu::Element formatGPU = gpu::Element(gpu::SCALAR, gpu::NUINT8, gpu::RGB);
#endif
        gpu::Element formatMip = gpu::Element(gpu::SCALAR, gpu::NUINT8, gpu::RGB);

        theTexture = (gpu::Texture::create2D(formatGPU, image.width(), image.height(), gpu::Sampler(gpu::Sampler::FILTER_MIN_MAG_MIP_LINEAR), DependencyManager::get<TextureCache>()->getDefaultTexture(ROUGHNESS_TEXTURE)));
        theTexture->assignStoredMip(0, formatMip, image.byteCount(), image.constBits());
        theTexture->autoGenerateMips(-1);

        // FIXME queue for transfer to GPU and block on completion
    }

    return theTexture;
}

gpu::Texture* NetworkTexture::createRoughnessTextureFromGlossImage(const QImage& srcImage, const std::string& srcImageName) {
    QImage image = srcImage;
    if (!image.hasAlphaChannel()) {
        if (image.format() != QImage::Format_RGB888) {
            image = image.convertToFormat(QImage::Format_RGB888);
        }
    } else {
        if (image.format() != QImage::Format_ARGB32) {
            image = image.convertToFormat(QImage::Format_ARGB32);
        }
    }

    // Gloss turned into Rough
    image.invertPixels(QImage::InvertRgba);
    
    image = image.convertToFormat(QImage::Format_Grayscale8);
    
    gpu::Texture* theTexture = nullptr;
    if ((image.width() > 0) && (image.height() > 0)) {
        
#ifdef COMPRESS_TEXTURES
        gpu::Element formatGPU = gpu::Element(gpu::SCALAR, gpu::NUINT8, gpu::COMPRESSED_R);
#else
        gpu::Element formatGPU = gpu::Element(gpu::SCALAR, gpu::NUINT8, gpu::RGB);
#endif
        gpu::Element formatMip = gpu::Element(gpu::SCALAR, gpu::NUINT8, gpu::RGB);

        theTexture = (gpu::Texture::create2D(formatGPU, image.width(), image.height(), gpu::Sampler(gpu::Sampler::FILTER_MIN_MAG_MIP_LINEAR), DependencyManager::get<TextureCache>()->getDefaultTexture(ROUGHNESS_TEXTURE)));
        theTexture->assignStoredMip(0, formatMip, image.byteCount(), image.constBits());
        theTexture->autoGenerateMips(-1);
        
        // FIXME queue for transfer to GPU and block on completion
    }
    
    return theTexture;
}

gpu::Texture* NetworkTexture::createMetallicTextureFromImage(const QImage& srcImage, const std::string& srcImageName) {
    QImage image = srcImage;
    if (!image.hasAlphaChannel()) {
        if (image.format() != QImage::Format_RGB888) {
            image = image.convertToFormat(QImage::Format_RGB888);
        }
    } else {
        if (image.format() != QImage::Format_ARGB32) {
            image = image.convertToFormat(QImage::Format_ARGB32);
        }
    }

    image = image.convertToFormat(QImage::Format_Grayscale8);

    gpu::Texture* theTexture = nullptr;
    if ((image.width() > 0) && (image.height() > 0)) {

#ifdef COMPRESS_TEXTURES
        gpu::Element formatGPU = gpu::Element(gpu::SCALAR, gpu::NUINT8, gpu::COMPRESSED_R);
#else
        gpu::Element formatGPU = gpu::Element(gpu::SCALAR, gpu::NUINT8, gpu::RGB);
#endif
        gpu::Element formatMip = gpu::Element(gpu::SCALAR, gpu::NUINT8, gpu::RGB);

        theTexture = (gpu::Texture::create2D(formatGPU, image.width(), image.height(), gpu::Sampler(gpu::Sampler::FILTER_MIN_MAG_MIP_LINEAR), DependencyManager::get<TextureCache>()->getDefaultTexture(METALLIC_TEXTURE)));
        theTexture->assignStoredMip(0, formatMip, image.byteCount(), image.constBits());
        theTexture->autoGenerateMips(-1);

        // FIXME queue for transfer to GPU and block on completion
    }

    return theTexture;
}

class CubeLayout {
public:
    int _widthRatio = 1;
    int _heightRatio = 1;
    
    class Face {
    public:
        int _x = 0;
        int _y = 0;
        bool _horizontalMirror = false;
        bool _verticalMirror = false;
        
        Face() {}
        Face(int x, int y, bool horizontalMirror, bool verticalMirror) : _x(x), _y(y), _horizontalMirror(horizontalMirror), _verticalMirror(verticalMirror) {}
    };
    
    Face _faceXPos;
    Face _faceXNeg;
    Face _faceYPos;
    Face _faceYNeg;
    Face _faceZPos;
    Face _faceZNeg;
    
    CubeLayout(int wr, int hr, Face fXP, Face fXN, Face fYP, Face fYN, Face fZP, Face fZN) :
    _widthRatio(wr),
    _heightRatio(hr),
    _faceXPos(fXP),
    _faceXNeg(fXN),
    _faceYPos(fYP),
    _faceYNeg(fYN),
    _faceZPos(fZP),
    _faceZNeg(fZN) {}


    static const CubeLayout CUBEMAP_LAYOUTS[];
    static const int NUM_CUBEMAP_LAYOUTS;

    static int findLayout(int width, int height) {
        // Find the layout of the cubemap in the 2D image
        int foundLayout = -1;
        for (int i = 0; i < NUM_CUBEMAP_LAYOUTS; i++) {
            if ((height * CUBEMAP_LAYOUTS[i]._widthRatio) == (width * CUBEMAP_LAYOUTS[i]._heightRatio)) {
                foundLayout = i;
                break;
            }
        }
        return foundLayout;
    }
};

const CubeLayout CubeLayout::CUBEMAP_LAYOUTS[] = {
    // Here is the expected layout for the faces in an image with the 1/6 aspect ratio:
    //
    //         WIDTH
    //       <------>
    //    ^  +------+
    //    |  |      |
    //    |  |  +X  |
    //    |  |      |
    //    H  +------+
    //    E  |      |
    //    I  |  -X  |
    //    G  |      |
    //    H  +------+
    //    T  |      |
    //    |  |  +Y  |
    //    |  |      |
    //    |  +------+
    //    |  |      |
    //    |  |  -Y  |
    //    |  |      |
    //    H  +------+
    //    E  |      |
    //    I  |  +Z  |
    //    G  |      |
    //    H  +------+
    //    T  |      |
    //    |  |  -Z  |
    //    |  |      |
    //    V  +------+
    //
    //    FaceWidth = width = height / 6
    { 1, 6,
    { 0, 0, true, false },
    { 0, 1, true, false },
    { 0, 2, false, true },
    { 0, 3, false, true },
    { 0, 4, true, false },
    { 0, 5, true, false }
    },

    // Here is the expected layout for the faces in an image with the 3/4 aspect ratio:
    //
    //       <-----------WIDTH----------->
    //    ^  +------+------+------+------+
    //    |  |      |      |      |      |
    //    |  |      |  +Y  |      |      |
    //    |  |      |      |      |      |
    //    H  +------+------+------+------+
    //    E  |      |      |      |      |
    //    I  |  -X  |  -Z  |  +X  |  +Z  |
    //    G  |      |      |      |      |
    //    H  +------+------+------+------+
    //    T  |      |      |      |      |
    //    |  |      |  -Y  |      |      |
    //    |  |      |      |      |      |
    //    V  +------+------+------+------+
    //
    //    FaceWidth = width / 4 = height / 3
    { 4, 3,
    { 2, 1, true, false },
    { 0, 1, true, false },
    { 1, 0, false, true },
    { 1, 2, false, true },
    { 3, 1, true, false },
    { 1, 1, true, false }
    },

    // Here is the expected layout for the faces in an image with the 4/3 aspect ratio:
    //
    //       <-------WIDTH-------->
    //    ^  +------+------+------+
    //    |  |      |      |      |
    //    |  |      |  +Y  |      |
    //    |  |      |      |      |
    //    H  +------+------+------+
    //    E  |      |      |      |
    //    I  |  -X  |  -Z  |  +X  |
    //    G  |      |      |      |
    //    H  +------+------+------+
    //    T  |      |      |      |
    //    |  |      |  -Y  |      |
    //    |  |      |      |      |
    //    |  +------+------+------+
    //    |  |      |      |      |
    //    |  |      |  +Z! |      | <+Z is upside down!
    //    |  |      |      |      |
    //    V  +------+------+------+
    //
    //    FaceWidth = width / 3 = height / 4
    { 3, 4,
    { 2, 1, true, false },
    { 0, 1, true, false },
    { 1, 0, false, true },
    { 1, 2, false, true },
    { 1, 3, false, true },
    { 1, 1, true, false }
    }
};
const int CubeLayout::NUM_CUBEMAP_LAYOUTS = sizeof(CubeLayout::CUBEMAP_LAYOUTS) / sizeof(CubeLayout);

gpu::Texture* NetworkTexture::processCubeTextureColorFromImage(const QImage& srcImage, const std::string& srcImageName, bool isLinear, bool doCompress, bool generateMips, bool generateIrradiance, const gpu::TexturePointer& defaultTexture) {

    bool validAlpha = false;
    bool alphaAsMask = true;
    QImage image = process2DImageColor(srcImage, validAlpha, alphaAsMask);

    gpu::Texture* theTexture = nullptr;
    if ((image.width() > 0) && (image.height() > 0)) {

        gpu::Element formatGPU;
        gpu::Element formatMip;
        defineColorTexelFormats(formatGPU, formatMip, image, isLinear, doCompress);

        // Find the layout of the cubemap in the 2D image
        int foundLayout = CubeLayout::findLayout(image.width(), image.height());
        
        std::vector<QImage> faces;
        // If found, go extract the faces as separate images
        if (foundLayout >= 0) {
            auto& layout = CubeLayout::CUBEMAP_LAYOUTS[foundLayout];
            int faceWidth = image.width() / layout._widthRatio;

            faces.push_back(image.copy(QRect(layout._faceXPos._x * faceWidth, layout._faceXPos._y * faceWidth, faceWidth, faceWidth)).mirrored(layout._faceXPos._horizontalMirror, layout._faceXPos._verticalMirror));
            faces.push_back(image.copy(QRect(layout._faceXNeg._x * faceWidth, layout._faceXNeg._y * faceWidth, faceWidth, faceWidth)).mirrored(layout._faceXNeg._horizontalMirror, layout._faceXNeg._verticalMirror));
            faces.push_back(image.copy(QRect(layout._faceYPos._x * faceWidth, layout._faceYPos._y * faceWidth, faceWidth, faceWidth)).mirrored(layout._faceYPos._horizontalMirror, layout._faceYPos._verticalMirror));
            faces.push_back(image.copy(QRect(layout._faceYNeg._x * faceWidth, layout._faceYNeg._y * faceWidth, faceWidth, faceWidth)).mirrored(layout._faceYNeg._horizontalMirror, layout._faceYNeg._verticalMirror));
            faces.push_back(image.copy(QRect(layout._faceZPos._x * faceWidth, layout._faceZPos._y * faceWidth, faceWidth, faceWidth)).mirrored(layout._faceZPos._horizontalMirror, layout._faceZPos._verticalMirror));
            faces.push_back(image.copy(QRect(layout._faceZNeg._x * faceWidth, layout._faceZNeg._y * faceWidth, faceWidth, faceWidth)).mirrored(layout._faceZNeg._horizontalMirror, layout._faceZNeg._verticalMirror));
        } else {
            qCDebug(modelnetworking) << "Failed to find a known cube map layout from this image:" << QString(srcImageName.c_str());
            return nullptr;
        }

        // If the 6 faces have been created go on and define the true Texture
        if (faces.size() == gpu::Texture::NUM_FACES_PER_TYPE[gpu::Texture::TEX_CUBE]) {
            theTexture = gpu::Texture::createCube(formatGPU, faces[0].width(), gpu::Sampler(gpu::Sampler::FILTER_MIN_MAG_MIP_LINEAR, gpu::Sampler::WRAP_CLAMP), defaultTexture);
            int f = 0;
            for (auto& face : faces) {
                theTexture->assignStoredMipFace(0, formatMip, face.byteCount(), face.constBits(), f);
                f++;
            }

            if (generateMips) {
                theTexture->autoGenerateMips(-1);
            }

            // Generate irradiance while we are at it
            if (generateIrradiance) {
                theTexture->generateIrradiance();
            }
        }
    }

    return theTexture;
}

gpu::Texture* NetworkTexture::createCubeTextureFromImage(const QImage& srcImage, const std::string& srcImageName) {
    return processCubeTextureColorFromImage(srcImage, srcImageName, false, true, true, true, DependencyManager::get<TextureCache>()->getDefaultTexture(CUBE_TEXTURE));
}
