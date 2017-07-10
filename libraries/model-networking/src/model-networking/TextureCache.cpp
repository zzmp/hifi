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

#include <QtConcurrent/QtConcurrentRun>

#include <QCryptographicHash>
#include <QImageReader>
#include <QRunnable>
#include <QThreadPool>
#include <QNetworkReply>
#include <QPainter>

#if DEBUG_DUMP_TEXTURE_LOADS
#include <QtCore/QFile>
#include <QtCore/QFileInfo>
#endif

#include <glm/glm.hpp>
#include <glm/gtc/random.hpp>

#include <gpu/Batch.h>

#include <image/Image.h>

#include <NumericalConstants.h>
#include <shared/NsightHelpers.h>

#include <Finally.h>
#include <Profile.h>

#include "NetworkLogging.h"
#include "ModelNetworkingLogging.h"
#include <Trace.h>
#include <StatTracker.h>

Q_LOGGING_CATEGORY(trace_resource_parse_image, "trace.resource.parse.image")
Q_LOGGING_CATEGORY(trace_resource_parse_image_raw, "trace.resource.parse.image.raw")
Q_LOGGING_CATEGORY(trace_resource_parse_image_ktx, "trace.resource.parse.image.ktx")

const std::string TextureCache::KTX_DIRNAME { "ktx_cache" };
const std::string TextureCache::KTX_EXT { "ktx" };

static const QString RESOURCE_SCHEME = "resource";
static const QUrl SPECTATOR_CAMERA_FRAME_URL("resource://spectatorCameraFrame");

static const float SKYBOX_LOAD_PRIORITY { 10.0f }; // Make sure skybox loads first
static const float HIGH_MIPS_LOAD_PRIORITY { 9.0f }; // Make sure high mips loads after skybox but before models

TextureCache::TextureCache() {
    _ktxCache->initialize();
    setUnusedResourceCacheSize(0);
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

        _permutationNormalTexture = gpu::Texture::create2D(gpu::Element(gpu::VEC3, gpu::NUINT8, gpu::RGB), 256, 2);
        _permutationNormalTexture->setStoredMipFormat(_permutationNormalTexture->getTexelFormat());
        _permutationNormalTexture->assignStoredMip(0, sizeof(data), data);
    }
    return _permutationNormalTexture;
}

const unsigned char OPAQUE_WHITE[] = { 0xFF, 0xFF, 0xFF, 0xFF };
const unsigned char OPAQUE_GRAY[] = { 0x80, 0x80, 0x80, 0xFF };
const unsigned char OPAQUE_BLUE[] = { 0x80, 0x80, 0xFF, 0xFF };
const unsigned char OPAQUE_BLACK[] = { 0x00, 0x00, 0x00, 0xFF };

const gpu::TexturePointer& TextureCache::getWhiteTexture() {
    if (!_whiteTexture) {
        _whiteTexture = gpu::Texture::createStrict(gpu::Element::COLOR_RGBA_32, 1, 1);
        _whiteTexture->setSource("TextureCache::_whiteTexture");
        _whiteTexture->setStoredMipFormat(_whiteTexture->getTexelFormat());
        _whiteTexture->assignStoredMip(0, sizeof(OPAQUE_WHITE), OPAQUE_WHITE);
    }
    return _whiteTexture;
}

const gpu::TexturePointer& TextureCache::getGrayTexture() {
    if (!_grayTexture) {
        _grayTexture = gpu::Texture::createStrict(gpu::Element::COLOR_RGBA_32, 1, 1);
        _grayTexture->setSource("TextureCache::_grayTexture");
        _grayTexture->setStoredMipFormat(_grayTexture->getTexelFormat());
        _grayTexture->assignStoredMip(0, sizeof(OPAQUE_GRAY), OPAQUE_GRAY);
    }
    return _grayTexture;
}

const gpu::TexturePointer& TextureCache::getBlueTexture() {
    if (!_blueTexture) {
        _blueTexture = gpu::Texture::createStrict(gpu::Element::COLOR_RGBA_32, 1, 1);
        _blueTexture->setSource("TextureCache::_blueTexture");
        _blueTexture->setStoredMipFormat(_blueTexture->getTexelFormat());
        _blueTexture->assignStoredMip(0, sizeof(OPAQUE_BLUE), OPAQUE_BLUE);
    }
    return _blueTexture;
}

const gpu::TexturePointer& TextureCache::getBlackTexture() {
    if (!_blackTexture) {
        _blackTexture = gpu::Texture::createStrict(gpu::Element::COLOR_RGBA_32, 1, 1);
        _blackTexture->setSource("TextureCache::_blackTexture");
        _blackTexture->setStoredMipFormat(_blackTexture->getTexelFormat());
        _blackTexture->assignStoredMip(0, sizeof(OPAQUE_BLACK), OPAQUE_BLACK);
    }
    return _blackTexture;
}

/// Extra data for creating textures.
class TextureExtra {
public:
    image::TextureUsage::Type type;
    const QByteArray& content;
    int maxNumPixels;
};

ScriptableResource* TextureCache::prefetch(const QUrl& url, int type, int maxNumPixels) {
    auto byteArray = QByteArray();
    TextureExtra extra = { (image::TextureUsage::Type)type, byteArray, maxNumPixels };
    return ResourceCache::prefetch(url, &extra);
}

NetworkTexturePointer TextureCache::getTexture(const QUrl& url, image::TextureUsage::Type type, const QByteArray& content, int maxNumPixels) {
    if (url.scheme() == RESOURCE_SCHEME) {
        return getResourceTexture(url);
    }
    TextureExtra extra = { type, content, maxNumPixels };
    return ResourceCache::getResource(url, QUrl(), &extra).staticCast<NetworkTexture>();
}

gpu::TexturePointer TextureCache::getTextureByHash(const std::string& hash) {
    std::weak_ptr<gpu::Texture> weakPointer;
    {
        std::unique_lock<std::mutex> lock(_texturesByHashesMutex);
        weakPointer = _texturesByHashes[hash];
    }
    return weakPointer.lock();
}

gpu::TexturePointer TextureCache::cacheTextureByHash(const std::string& hash, const gpu::TexturePointer& texture) {
    gpu::TexturePointer result;
    {
        std::unique_lock<std::mutex> lock(_texturesByHashesMutex);
        result = _texturesByHashes[hash].lock();
        if (!result) {
            _texturesByHashes[hash] = texture;
            result = texture;
        } else {
            qCWarning(modelnetworking) << "QQQ Swapping out texture with previous live texture in hash " << hash.c_str();
        }
    }
    return result;
}

gpu::TexturePointer getFallbackTextureForType(image::TextureUsage::Type type) {
    gpu::TexturePointer result;
    auto textureCache = DependencyManager::get<TextureCache>();
    // Since this can be called on a background thread, there's a chance that the cache 
    // will be destroyed by the time we request it
    if (!textureCache) {
        return result;
    }
    switch (type) {
        case image::TextureUsage::DEFAULT_TEXTURE:
        case image::TextureUsage::ALBEDO_TEXTURE:
        case image::TextureUsage::ROUGHNESS_TEXTURE:
        case image::TextureUsage::OCCLUSION_TEXTURE:
            result = textureCache->getWhiteTexture();
            break;

        case image::TextureUsage::NORMAL_TEXTURE:
            result = textureCache->getBlueTexture();
            break;

        case image::TextureUsage::EMISSIVE_TEXTURE:
        case image::TextureUsage::LIGHTMAP_TEXTURE:
            result = textureCache->getBlackTexture();
            break;

        case image::TextureUsage::BUMP_TEXTURE:
        case image::TextureUsage::SPECULAR_TEXTURE:
        case image::TextureUsage::GLOSS_TEXTURE:
        case image::TextureUsage::CUBE_TEXTURE:
        case image::TextureUsage::STRICT_TEXTURE:
        default:
            break;
    }
    return result;
}

/// Returns a texture version of an image file
gpu::TexturePointer TextureCache::getImageTexture(const QString& path, image::TextureUsage::Type type, QVariantMap options) {
    QImage image = QImage(path);
    auto loader = image::TextureUsage::getTextureLoaderForType(type, options);
    return gpu::TexturePointer(loader(image, QUrl::fromLocalFile(path).fileName().toStdString()));
}

QSharedPointer<Resource> TextureCache::createResource(const QUrl& url, const QSharedPointer<Resource>& fallback,
    const void* extra) {
    const TextureExtra* textureExtra = static_cast<const TextureExtra*>(extra);
    auto type = textureExtra ? textureExtra->type : image::TextureUsage::DEFAULT_TEXTURE;
    auto content = textureExtra ? textureExtra->content : QByteArray();
    auto maxNumPixels = textureExtra ? textureExtra->maxNumPixels : ABSOLUTE_MAX_TEXTURE_NUM_PIXELS;
    NetworkTexture* texture = new NetworkTexture(url, type, content, maxNumPixels);
    return QSharedPointer<Resource>(texture, &Resource::deleter);
}

NetworkTexture::NetworkTexture(const QUrl& url) :
Resource(url),
_type(),
_sourceIsKTX(false),
_maxNumPixels(100)
{
    _textureSource = std::make_shared<gpu::TextureSource>();
    _lowestRequestedMipLevel = 0;
    _loaded = true;
}


NetworkTexture::NetworkTexture(const QUrl& url, image::TextureUsage::Type type, const QByteArray& content, int maxNumPixels) :
    Resource(url),
    _type(type),
    _sourceIsKTX(url.path().endsWith(".ktx")),
    _maxNumPixels(maxNumPixels)
{
    _textureSource = std::make_shared<gpu::TextureSource>();
    _lowestRequestedMipLevel = 0;

    if (type == image::TextureUsage::CUBE_TEXTURE) {
        setLoadPriority(this, SKYBOX_LOAD_PRIORITY);
    } else if (_sourceIsKTX) {
        setLoadPriority(this, HIGH_MIPS_LOAD_PRIORITY);
    }

    if (!url.isValid()) {
        _loaded = true;
    }

    // if we have content, load it after we have our self pointer
    if (!content.isEmpty()) {
        _startedLoading = true;
        QMetaObject::invokeMethod(this, "loadContent", Qt::QueuedConnection, Q_ARG(const QByteArray&, content));
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
        finishedLoading(true);
    } else {
        _width = _height = 0;
        finishedLoading(false);
    }

    emit networkTextureCreated(qWeakPointerCast<NetworkTexture, Resource> (_self));
}

gpu::TexturePointer NetworkTexture::getFallbackTexture() const {
    return getFallbackTextureForType(_type);
}

class ImageReader : public QRunnable {
public:
    ImageReader(const QWeakPointer<Resource>& resource, const QUrl& url,
                const QByteArray& data, int maxNumPixels);
    void run() override final;
    void read();

private:
    static void listSupportedImageFormats();

    QWeakPointer<Resource> _resource;
    QUrl _url;
    QByteArray _content;
    int _maxNumPixels;
};

NetworkTexture::~NetworkTexture() {
    if (_ktxHeaderRequest || _ktxMipRequest) {
        if (_ktxHeaderRequest) {
            _ktxHeaderRequest->disconnect(this);
            _ktxHeaderRequest->deleteLater();
            _ktxHeaderRequest = nullptr;
        }
        if (_ktxMipRequest) {
            _ktxMipRequest->disconnect(this);
            _ktxMipRequest->deleteLater();
            _ktxMipRequest = nullptr;
        }
        TextureCache::requestCompleted(_self);
    }
}

const uint16_t NetworkTexture::NULL_MIP_LEVEL = std::numeric_limits<uint16_t>::max();
void NetworkTexture::makeRequest() {
    if (!_sourceIsKTX) {
        Resource::makeRequest();
        return;
    } 

    // We special-handle ktx requests to run 2 concurrent requests right off the bat
    PROFILE_ASYNC_BEGIN(resource, "Resource:" + getType(), QString::number(_requestID), { { "url", _url.toString() }, { "activeURL", _activeUrl.toString() } });

    if (_ktxResourceState == PENDING_INITIAL_LOAD) {
        _ktxResourceState = LOADING_INITIAL_DATA;

        // Add a fragment to the base url so we can identify the section of the ktx being requested when debugging
        // The actual requested url is _activeUrl and will not contain the fragment
        _url.setFragment("head");
        _ktxHeaderRequest = DependencyManager::get<ResourceManager>()->createResourceRequest(this, _activeUrl);

        if (!_ktxHeaderRequest) {
            qCDebug(networking).noquote() << "Failed to get request for" << _url.toDisplayString();

            PROFILE_ASYNC_END(resource, "Resource:" + getType(), QString::number(_requestID));
            return;
        }

        ByteRange range;
        range.fromInclusive = 0;
        range.toExclusive = 1000;
        _ktxHeaderRequest->setByteRange(range);

        emit loading();

        connect(_ktxHeaderRequest, &ResourceRequest::finished, this, &NetworkTexture::ktxInitialDataRequestFinished);

        _bytesReceived = _bytesTotal = _bytes = 0;

        _ktxHeaderRequest->send();

        startMipRangeRequest(NULL_MIP_LEVEL, NULL_MIP_LEVEL);
    } else if (_ktxResourceState == PENDING_MIP_REQUEST) {
        if (_lowestKnownPopulatedMip > 0) {
            _ktxResourceState = REQUESTING_MIP;

            // Add a fragment to the base url so we can identify the section of the ktx being requested when debugging
            // The actual requested url is _activeUrl and will not contain the fragment
            uint16_t nextMip = _lowestKnownPopulatedMip - 1;
            _url.setFragment(QString::number(nextMip));
            startMipRangeRequest(nextMip, nextMip);
        }
    } else {
        qWarning(networking) << "NetworkTexture::makeRequest() called while not in a valid state: " << _ktxResourceState;
    }

}

void NetworkTexture::startRequestForNextMipLevel() {
    auto self = _self.lock();
    if (!self) {
        return;
    }

    auto texture = _textureSource->getGPUTexture();
    if (!texture || _ktxResourceState != WAITING_FOR_MIP_REQUEST) {
        return;
    }

    _lowestKnownPopulatedMip = texture->minAvailableMipLevel();
    if (_lowestRequestedMipLevel < _lowestKnownPopulatedMip) {
        _ktxResourceState = PENDING_MIP_REQUEST;

        init(false);
        float priority = -(float)_originalKtxDescriptor->header.numberOfMipmapLevels + (float)_lowestKnownPopulatedMip;
        setLoadPriority(this, priority);
        _url.setFragment(QString::number(_lowestKnownPopulatedMip - 1));
        TextureCache::attemptRequest(self);
    }
}

// Load mips in the range [low, high] (inclusive)
void NetworkTexture::startMipRangeRequest(uint16_t low, uint16_t high) {
    if (_ktxMipRequest) {
        return;
    }

    bool isHighMipRequest = low == NULL_MIP_LEVEL && high == NULL_MIP_LEVEL;

    _ktxMipRequest = DependencyManager::get<ResourceManager>()->createResourceRequest(this, _activeUrl);

    if (!_ktxMipRequest) {
        qCWarning(networking).noquote() << "Failed to get request for" << _url.toDisplayString();

        PROFILE_ASYNC_END(resource, "Resource:" + getType(), QString::number(_requestID));
        return;
    }

    _ktxMipLevelRangeInFlight = { low, high };
    if (isHighMipRequest) {
        static const int HIGH_MIP_MAX_SIZE = 5516;
        // This is a special case where we load the high 7 mips
        ByteRange range;
        range.fromInclusive = -HIGH_MIP_MAX_SIZE;
        _ktxMipRequest->setByteRange(range);

        connect(_ktxMipRequest, &ResourceRequest::finished, this, &NetworkTexture::ktxInitialDataRequestFinished);
    } else {
        ByteRange range;
        range.fromInclusive = ktx::KTX_HEADER_SIZE + _originalKtxDescriptor->header.bytesOfKeyValueData
                              + _originalKtxDescriptor->images[low]._imageOffset + ktx::IMAGE_SIZE_WIDTH;
        range.toExclusive = ktx::KTX_HEADER_SIZE + _originalKtxDescriptor->header.bytesOfKeyValueData
                              + _originalKtxDescriptor->images[high + 1]._imageOffset;
        _ktxMipRequest->setByteRange(range);

        connect(_ktxMipRequest, &ResourceRequest::finished, this, &NetworkTexture::ktxMipRequestFinished);
    }

    _ktxMipRequest->send();
}


// This is called when the header or top mips have been loaded
void NetworkTexture::ktxInitialDataRequestFinished() {
    if (!_ktxHeaderRequest || _ktxHeaderRequest->getState() != ResourceRequest::Finished ||
        !_ktxMipRequest ||  _ktxMipRequest->getState() != ResourceRequest::Finished) {
        // Wait for both request to be finished
        return;
    }

    Q_ASSERT(_ktxResourceState == LOADING_INITIAL_DATA);
    Q_ASSERT_X(_ktxHeaderRequest && _ktxMipRequest, __FUNCTION__, "Request should not be null while in ktxInitialDataRequestFinished");

    PROFILE_ASYNC_END(resource, "Resource:" + getType(), QString::number(_requestID), {
        { "from_cache", _ktxHeaderRequest->loadedFromCache() },
        { "size_mb", _bytesTotal / 1000000.0 }
    });

    PROFILE_RANGE_EX(resource_parse_image, __FUNCTION__, 0xffff0000, 0, { { "url", _url.toString() } });

    setSize(_bytesTotal);

    TextureCache::requestCompleted(_self);

    auto result = _ktxHeaderRequest->getResult();
    if (result == ResourceRequest::Success) {
        result = _ktxMipRequest->getResult();
    }

    if (result == ResourceRequest::Success) {
        auto extraInfo = _url == _activeUrl ? "" : QString(", %1").arg(_activeUrl.toDisplayString());
        qCDebug(networking).noquote() << QString("Request finished for %1%2").arg(_url.toDisplayString(), extraInfo);

        _ktxHeaderData = _ktxHeaderRequest->getData();
        _ktxHighMipData = _ktxMipRequest->getData();
        handleFinishedInitialLoad();
    } else {
        if (handleFailedRequest(result)) {
            _ktxResourceState = PENDING_INITIAL_LOAD;
        } else {
            _ktxResourceState = FAILED_TO_LOAD;
        }
    }

    _ktxHeaderRequest->disconnect(this);
    _ktxHeaderRequest->deleteLater();
    _ktxHeaderRequest = nullptr;
    _ktxMipRequest->disconnect(this);
    _ktxMipRequest->deleteLater();
    _ktxMipRequest = nullptr;
}

void NetworkTexture::ktxMipRequestFinished() {
    Q_ASSERT_X(_ktxMipRequest, __FUNCTION__, "Request should not be null while in ktxMipRequestFinished");
    Q_ASSERT(_ktxResourceState == REQUESTING_MIP);

    PROFILE_ASYNC_END(resource, "Resource:" + getType(), QString::number(_requestID), {
        { "from_cache", _ktxMipRequest->loadedFromCache() },
        { "size_mb", _bytesTotal / 1000000.0 }
    });

    PROFILE_RANGE_EX(resource_parse_image, __FUNCTION__, 0xffff0000, 0, { { "url", _url.toString() } });


    setSize(_bytesTotal);

    if (!_ktxMipRequest || _ktxMipRequest != sender()) {
        // This can happen in the edge case that a request is timed out, but a `finished` signal is emitted before it is deleted.
        qWarning(networking) << "Received signal NetworkTexture::ktxMipRequestFinished from ResourceRequest that is not the current"
            << " request: " << sender() << ", " << _ktxMipRequest;
        return;
    }

    TextureCache::requestCompleted(_self);

    auto result = _ktxMipRequest->getResult();
    if (result == ResourceRequest::Success) {
        auto extraInfo = _url == _activeUrl ? "" : QString(", %1").arg(_activeUrl.toDisplayString());
        qCDebug(networking).noquote() << QString("Request finished for %1%2").arg(_url.toDisplayString(), extraInfo);

        if (_ktxResourceState == REQUESTING_MIP) {
            Q_ASSERT(_ktxMipLevelRangeInFlight.first != NULL_MIP_LEVEL);
            Q_ASSERT(_ktxMipLevelRangeInFlight.second - _ktxMipLevelRangeInFlight.first == 0);

            _ktxResourceState = WAITING_FOR_MIP_REQUEST;

            auto self = _self;
            auto url = _url;
            auto data = _ktxMipRequest->getData();
            auto mipLevel = _ktxMipLevelRangeInFlight.first;
            auto texture = _textureSource->getGPUTexture();
            DependencyManager::get<StatTracker>()->incrementStat("PendingProcessing");
            QtConcurrent::run(QThreadPool::globalInstance(), [self, data, mipLevel, url, texture] {
                PROFILE_RANGE_EX(resource_parse_image, "NetworkTexture - Processing Mip Data", 0xffff0000, 0, { { "url", url.toString() } });
                DependencyManager::get<StatTracker>()->decrementStat("PendingProcessing");
                CounterStat counter("Processing");

                auto originalPriority = QThread::currentThread()->priority();
                if (originalPriority == QThread::InheritPriority) {
                    originalPriority = QThread::NormalPriority;
                }
                QThread::currentThread()->setPriority(QThread::LowPriority);
                Finally restorePriority([originalPriority] { QThread::currentThread()->setPriority(originalPriority); });

                auto resource = self.lock();
                if (!resource) {
                    // Resource no longer exists, bail
                    return;
                }

                Q_ASSERT_X(texture, "Async - NetworkTexture::ktxMipRequestFinished", "NetworkTexture should have been assigned a GPU texture by now.");

                texture->assignStoredMip(mipLevel, data.size(), reinterpret_cast<const uint8_t*>(data.data()));

                QMetaObject::invokeMethod(resource.data(), "setImage",
                    Q_ARG(gpu::TexturePointer, texture),
                    Q_ARG(int, texture->getWidth()),
                    Q_ARG(int, texture->getHeight()));

                QMetaObject::invokeMethod(resource.data(), "startRequestForNextMipLevel");
            });
        } else {
            qWarning(networking) << "Mip request finished in an unexpected state: " << _ktxResourceState;
            finishedLoading(false);
        }
    } else {
        if (handleFailedRequest(result)) {
            _ktxResourceState = PENDING_MIP_REQUEST;
        } else {
            _ktxResourceState = FAILED_TO_LOAD;
        }
    }

    _ktxMipRequest->disconnect(this);
    _ktxMipRequest->deleteLater();
    _ktxMipRequest = nullptr;
}

// This is called when the header and top mips have been loaded
void NetworkTexture::handleFinishedInitialLoad() {
    Q_ASSERT(_ktxResourceState == LOADING_INITIAL_DATA);
    Q_ASSERT(!_ktxHeaderData.isEmpty() && !_ktxHighMipData.isEmpty());

    // create ktx...
    auto ktxHeaderData = _ktxHeaderData;
    auto ktxHighMipData = _ktxHighMipData;
    _ktxHeaderData.clear();
    _ktxHighMipData.clear();

    _ktxResourceState = WAITING_FOR_MIP_REQUEST;

    auto self = _self;
    auto url = _url;
    DependencyManager::get<StatTracker>()->incrementStat("PendingProcessing");
    QtConcurrent::run(QThreadPool::globalInstance(), [self, ktxHeaderData, ktxHighMipData, url] {
        PROFILE_RANGE_EX(resource_parse_image, "NetworkTexture - Processing Initial Data", 0xffff0000, 0, { { "url", url.toString() } });
        DependencyManager::get<StatTracker>()->decrementStat("PendingProcessing");
        CounterStat counter("Processing");

        auto originalPriority = QThread::currentThread()->priority();
        if (originalPriority == QThread::InheritPriority) {
            originalPriority = QThread::NormalPriority;
        }
        QThread::currentThread()->setPriority(QThread::LowPriority);
        Finally restorePriority([originalPriority] { QThread::currentThread()->setPriority(originalPriority); });

        auto resource = self.lock();
        if (!resource) {
            // Resource no longer exists, bail
            return;
        }

        auto header = reinterpret_cast<const ktx::Header*>(ktxHeaderData.data());

        if (!ktx::checkIdentifier(header->identifier)) {
            qWarning() << "Cannot load " << url << ", invalid header identifier";
            QMetaObject::invokeMethod(resource.data(), "setImage",
                Q_ARG(gpu::TexturePointer, nullptr),
                Q_ARG(int, 0),
                Q_ARG(int, 0));
            return;
        }

        auto kvSize = header->bytesOfKeyValueData;
        if (kvSize > (ktxHeaderData.size() - ktx::KTX_HEADER_SIZE)) {
            qWarning() << "Cannot load " << url << ", did not receive all kv data with initial request";
            QMetaObject::invokeMethod(resource.data(), "setImage",
                Q_ARG(gpu::TexturePointer, nullptr),
                Q_ARG(int, 0),
                Q_ARG(int, 0));
            return;
        }

        auto keyValues = ktx::KTX::parseKeyValues(header->bytesOfKeyValueData, reinterpret_cast<const ktx::Byte*>(ktxHeaderData.data()) + ktx::KTX_HEADER_SIZE);

        auto imageDescriptors = header->generateImageDescriptors();
        if (imageDescriptors.size() == 0) {
            qWarning(networking) << "Failed to process ktx file " << url;
            QMetaObject::invokeMethod(resource.data(), "setImage",
                Q_ARG(gpu::TexturePointer, nullptr),
                Q_ARG(int, 0),
                Q_ARG(int, 0));
            return;
        }
        auto originalKtxDescriptor = new ktx::KTXDescriptor(*header, keyValues, imageDescriptors);
        QMetaObject::invokeMethod(resource.data(), "setOriginalDescriptor",
            Q_ARG(ktx::KTXDescriptor*, originalKtxDescriptor));

        // Create bare ktx in memory
        auto found = std::find_if(keyValues.begin(), keyValues.end(), [](const ktx::KeyValue& val) -> bool {
            return val._key.compare(gpu::SOURCE_HASH_KEY) == 0;
        });
        std::string filename;
        std::string hash;
        if (found == keyValues.end() || found->_value.size() != gpu::SOURCE_HASH_BYTES) {
            qWarning("Invalid source hash key found, bailing");
            QMetaObject::invokeMethod(resource.data(), "setImage",
                Q_ARG(gpu::TexturePointer, nullptr),
                Q_ARG(int, 0),
                Q_ARG(int, 0));
            return;
        } else {
            // at this point the source hash is in binary 16-byte form
            // and we need it in a hexadecimal string
            auto binaryHash = QByteArray(reinterpret_cast<char*>(found->_value.data()), gpu::SOURCE_HASH_BYTES);
            hash = filename = binaryHash.toHex().toStdString();
        }

        auto textureCache = DependencyManager::get<TextureCache>();

        gpu::TexturePointer texture = textureCache->getTextureByHash(hash);

        if (!texture) {
            auto ktxFile = textureCache->_ktxCache->getFile(hash);
            if (ktxFile) {
                texture = gpu::Texture::unserialize(ktxFile);
                if (texture) {
                    texture = textureCache->cacheTextureByHash(hash, texture);
                }
            }
        }

        if (!texture) {

            auto memKtx = ktx::KTX::createBare(*header, keyValues);
            if (!memKtx) {
                qWarning() << " Ktx could not be created, bailing";
                QMetaObject::invokeMethod(resource.data(), "setImage",
                    Q_ARG(gpu::TexturePointer, nullptr),
                    Q_ARG(int, 0),
                    Q_ARG(int, 0));
                return;
            }

            // Move ktx to file
            const char* data = reinterpret_cast<const char*>(memKtx->_storage->data());
            size_t length = memKtx->_storage->size();
            cache::FilePointer file;
            auto& ktxCache = textureCache->_ktxCache;
            if (!memKtx || !(file = ktxCache->writeFile(data, KTXCache::Metadata(filename, length)))) {
                qCWarning(modelnetworking) << url << " failed to write cache file";
                QMetaObject::invokeMethod(resource.data(), "setImage",
                    Q_ARG(gpu::TexturePointer, nullptr),
                    Q_ARG(int, 0),
                    Q_ARG(int, 0));
                return;
            }

            auto newKtxDescriptor = memKtx->toDescriptor();

            texture = gpu::Texture::build(newKtxDescriptor);
            texture->setKtxBacking(file);
            texture->setSource(filename);

            auto& images = originalKtxDescriptor->images;
            size_t imageSizeRemaining = ktxHighMipData.size();
            const uint8_t* ktxData = reinterpret_cast<const uint8_t*>(ktxHighMipData.data());
            ktxData += ktxHighMipData.size();
            // TODO Move image offset calculation to ktx ImageDescriptor
            for (int level = static_cast<int>(images.size()) - 1; level >= 0; --level) {
                auto& image = images[level];
                if (image._imageSize > imageSizeRemaining) {
                    break;
                }
                ktxData -= image._imageSize;
                texture->assignStoredMip(static_cast<gpu::uint16>(level), image._imageSize, ktxData);
                ktxData -= ktx::IMAGE_SIZE_WIDTH;
                imageSizeRemaining -= (image._imageSize + ktx::IMAGE_SIZE_WIDTH);
            }

            // We replace the texture with the one stored in the cache.  This deals with the possible race condition of two different
            // images with the same hash being loaded concurrently.  Only one of them will make it into the cache by hash first and will
            // be the winner
            texture = textureCache->cacheTextureByHash(filename, texture);
        }

        QMetaObject::invokeMethod(resource.data(), "setImage",
            Q_ARG(gpu::TexturePointer, texture),
            Q_ARG(int, texture->getWidth()),
            Q_ARG(int, texture->getHeight()));

        QMetaObject::invokeMethod(resource.data(), "startRequestForNextMipLevel");
    });
}

void NetworkTexture::downloadFinished(const QByteArray& data) {
    loadContent(data);
}

void NetworkTexture::loadContent(const QByteArray& content) {
    if (_sourceIsKTX) {
        assert(false);
        return;
    }

    QThreadPool::globalInstance()->start(new ImageReader(_self, _url, content, _maxNumPixels));
}

void NetworkTexture::refresh() {
    if ((_ktxHeaderRequest || _ktxMipRequest) && !_loaded && !_failedToLoad) {
        return;
    }
    if (_ktxHeaderRequest || _ktxMipRequest) {
        if (_ktxHeaderRequest) {
            _ktxHeaderRequest->disconnect(this);
            _ktxHeaderRequest->deleteLater();
            _ktxHeaderRequest = nullptr;
        }
        if (_ktxMipRequest) {
            _ktxMipRequest->disconnect(this);
            _ktxMipRequest->deleteLater();
            _ktxMipRequest = nullptr;
        }
        TextureCache::requestCompleted(_self);
    }

    _ktxResourceState = PENDING_INITIAL_LOAD;
    Resource::refresh();
}

ImageReader::ImageReader(const QWeakPointer<Resource>& resource, const QUrl& url, const QByteArray& data, int maxNumPixels) :
    _resource(resource),
    _url(url),
    _content(data),
    _maxNumPixels(maxNumPixels)
{
    DependencyManager::get<StatTracker>()->incrementStat("PendingProcessing");
    listSupportedImageFormats();

#if DEBUG_DUMP_TEXTURE_LOADS
    static auto start = usecTimestampNow() / USECS_PER_MSEC;
    auto now = usecTimestampNow() / USECS_PER_MSEC - start;
    QString urlStr = _url.toString();
    auto dot = urlStr.lastIndexOf(".");
    QString outFileName = QString(QCryptographicHash::hash(urlStr.toLocal8Bit(), QCryptographicHash::Md5).toHex()) + urlStr.right(urlStr.length() - dot);
    QFile loadRecord("h:/textures/loads.txt");
    loadRecord.open(QFile::Text | QFile::Append | QFile::ReadWrite);
    loadRecord.write(QString("%1 %2\n").arg(now).arg(outFileName).toLocal8Bit());
    outFileName = "h:/textures/" + outFileName;
    QFileInfo outInfo(outFileName);
    if (!outInfo.exists()) {
        QFile outFile(outFileName);
        outFile.open(QFile::WriteOnly | QFile::Truncate);
        outFile.write(data);
        outFile.close();
    }
#endif
}

void ImageReader::listSupportedImageFormats() {
    static std::once_flag once;
    std::call_once(once, []{
        auto supportedFormats = QImageReader::supportedImageFormats();
        qCDebug(modelnetworking) << "List of supported Image formats:" << supportedFormats.join(", ");
    });
}

void ImageReader::run() {
    PROFILE_RANGE_EX(resource_parse_image, __FUNCTION__, 0xffff0000, 0, { { "url", _url.toString() } });
    DependencyManager::get<StatTracker>()->decrementStat("PendingProcessing");
    CounterStat counter("Processing");

    auto originalPriority = QThread::currentThread()->priority();
    if (originalPriority == QThread::InheritPriority) {
        originalPriority = QThread::NormalPriority;
    }
    QThread::currentThread()->setPriority(QThread::LowPriority);
    Finally restorePriority([originalPriority] { QThread::currentThread()->setPriority(originalPriority); });

    read();
}

void ImageReader::read() {
    auto resource = _resource.lock(); // to ensure the resource is still needed
    if (!resource) {
        qCWarning(modelnetworking) << "Abandoning load of" << _url << "; could not get strong ref";
        return;
    }
    auto networkTexture = resource.staticCast<NetworkTexture>();

    // Hash the source image to for KTX caching
    std::string hash;
    {
        QCryptographicHash hasher(QCryptographicHash::Md5);
        hasher.addData(_content);
        hash = hasher.result().toHex().toStdString();
    }

    // Maybe load from cache
    auto textureCache = DependencyManager::get<TextureCache>();
    if (textureCache) {
        // If we already have a live texture with the same hash, use it
        auto texture = textureCache->getTextureByHash(hash);

        // If there is no live texture, check if there's an existing KTX file
        if (!texture) {
            auto ktxFile = textureCache->_ktxCache->getFile(hash);
            if (ktxFile) {
                texture = gpu::Texture::unserialize(ktxFile);
                if (texture) {
                    texture = textureCache->cacheTextureByHash(hash, texture);
                } else {
                    qCWarning(modelnetworking) << "Invalid cached KTX " << _url << " under hash " << hash.c_str() << ", recreating...";
                }
            }
        }

        // If we found the texture either because it's in use or via KTX deserialization, 
        // set the image and return immediately.
        if (texture) {
            QMetaObject::invokeMethod(resource.data(), "setImage",
                                      Q_ARG(gpu::TexturePointer, texture),
                                      Q_ARG(int, texture->getWidth()),
                                      Q_ARG(int, texture->getHeight()));
            return;
        }
    }

    // Proccess new texture
    gpu::TexturePointer texture;
    {
        PROFILE_RANGE_EX(resource_parse_image_raw, __FUNCTION__, 0xffff0000, 0);
        texture = image::processImage(_content, _url.toString().toStdString(), _maxNumPixels, networkTexture->getTextureType());

        if (!texture) {
            qCWarning(modelnetworking) << "Could not process:" << _url;
            QMetaObject::invokeMethod(resource.data(), "setImage",
                                      Q_ARG(gpu::TexturePointer, texture),
                                      Q_ARG(int, 0),
                                      Q_ARG(int, 0));
            return;
        }

        texture->setSourceHash(hash);
        texture->setFallbackTexture(networkTexture->getFallbackTexture());
    }

    // Save the image into a KTXFile
    if (texture && textureCache) {
        auto memKtx = gpu::Texture::serialize(*texture);

        // Move the texture into a memory mapped file
        if (memKtx) {
            const char* data = reinterpret_cast<const char*>(memKtx->_storage->data());
            size_t length = memKtx->_storage->size();
            auto& ktxCache = textureCache->_ktxCache;
            auto file = ktxCache->writeFile(data, KTXCache::Metadata(hash, length));
            if (!file) {
                qCWarning(modelnetworking) << _url << "file cache failed";
            } else {
                texture->setKtxBacking(file);
            }
        } else {
            qCWarning(modelnetworking) << "Unable to serialize texture to KTX " << _url;
        }

        // We replace the texture with the one stored in the cache.  This deals with the possible race condition of two different 
        // images with the same hash being loaded concurrently.  Only one of them will make it into the cache by hash first and will
        // be the winner
        texture = textureCache->cacheTextureByHash(hash, texture);
    }

    QMetaObject::invokeMethod(resource.data(), "setImage",
                                Q_ARG(gpu::TexturePointer, texture),
                                Q_ARG(int, texture->getWidth()),
                                Q_ARG(int, texture->getHeight()));
}


NetworkTexturePointer TextureCache::getResourceTexture(QUrl resourceTextureUrl) {
    gpu::TexturePointer texture;
    if (resourceTextureUrl == SPECTATOR_CAMERA_FRAME_URL) {
        if (!_spectatorCameraNetworkTexture) {
            _spectatorCameraNetworkTexture.reset(new NetworkTexture(resourceTextureUrl));
        }
        texture = _spectatorCameraFramebuffer->getRenderBuffer(0);
        if (texture) {
            _spectatorCameraNetworkTexture->setImage(texture, texture->getWidth(), texture->getHeight());
            return _spectatorCameraNetworkTexture;
        }
    }

    return NetworkTexturePointer();
}

const gpu::FramebufferPointer& TextureCache::getSpectatorCameraFramebuffer() {
    if (!_spectatorCameraFramebuffer) {
        resetSpectatorCameraFramebuffer(2048, 1024);
    }
    return _spectatorCameraFramebuffer;
}

void TextureCache::resetSpectatorCameraFramebuffer(int width, int height) {
    _spectatorCameraFramebuffer.reset(gpu::Framebuffer::create("spectatorCamera", gpu::Element::COLOR_SRGBA_32, width, height));
    _spectatorCameraNetworkTexture.reset();
}
