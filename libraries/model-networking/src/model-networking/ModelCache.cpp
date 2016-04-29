//
//  ModelCache.cpp
//  libraries/model-networking
//
//  Created by Zach Pomerantz on 3/15/16.
//  Copyright 2016 High Fidelity, Inc.
//
//  Distributed under the Apache License, Version 2.0.
//  See the accompanying file LICENSE or http://www.apache.org/licenses/LICENSE-2.0.html
//

#include "ModelCache.h"
#include <Finally.h>
#include <FSTReader.h>
#include "FBXReader.h"
#include "OBJReader.h"

#include <gpu/Batch.h>
#include <gpu/Stream.h>

#include <QThreadPool>

#include "ModelNetworkingLogging.h"

class GeometryReader;

class GeometryExtra {
public:
    const QVariantHash& mapping;
    const QUrl& textureBaseUrl;
};

QUrl resolveTextureBaseUrl(const QUrl& url, const QUrl& textureBaseUrl) {
    return textureBaseUrl.isValid() ? textureBaseUrl : url;
}

class GeometryMappingResource : public GeometryResource {
    Q_OBJECT
public:
    GeometryMappingResource(const QUrl& url) : GeometryResource(url) {};

    virtual void downloadFinished(const QByteArray& data) override;

private slots:
    void onGeometryMappingLoaded(bool success);

private:
    GeometryResource::Pointer _geometryResource;
    QMetaObject::Connection _connection;
};

void GeometryMappingResource::downloadFinished(const QByteArray& data) {
    auto mapping = FSTReader::readMapping(data);

    QString filename = mapping.value("filename").toString();
    if (filename.isNull()) {
        qCDebug(modelnetworking) << "Mapping file" << _url << "has no \"filename\" field";
        finishedLoading(false);
    } else {
        QUrl url = _url.resolved(filename);

        QString texdir = mapping.value("texdir").toString();
        if (!texdir.isNull()) {
            if (!texdir.endsWith('/')) {
                texdir += '/';
            }
            _textureBaseUrl = resolveTextureBaseUrl(url, _url.resolved(texdir));
        }

        auto modelCache = DependencyManager::get<ModelCache>();
        GeometryExtra extra{ mapping, _textureBaseUrl };

        // Get the raw GeometryResource, not the wrapped NetworkGeometry
        _geometryResource = modelCache->getResource(url, QUrl(), false, &extra).staticCast<GeometryResource>();
        // Avoid caching the nested resource - its reference will be held by the parent
        _geometryResource->_isCacheable = false;

        if (_geometryResource->isLoaded()) {
            onGeometryMappingLoaded(!_geometryResource->getURL().isEmpty());
        } else {
            if (_connection) {
                disconnect(_connection);
            }

            _connection = connect(_geometryResource.data(), &Resource::finished,
                                  this, &GeometryMappingResource::onGeometryMappingLoaded);
        }
    }
}

void GeometryMappingResource::onGeometryMappingLoaded(bool success) {
    if (success && _geometryResource) {
        // Copy over the geometry
        _joints = _geometryResource->_joints;
        _sittingPoints = _geometryResource->_sittingPoints;
        _shapes = _geometryResource->_shapes;
        _meshes = _geometryResource->_meshes;
        _materials = _geometryResource->_materials;

        // Swap the inlinedTextures, to prevent purging them on the _geometryResource dtor
        _inlinedTextures.swap(_geometryResource->_inlinedTextures);
        // Connect the inlinedTextures to this resource
        auto textureCache = DependencyManager::get<TextureCache>();
        for (const QUrl& textureUrl : _inlinedTextures) {
            auto texture = textureCache->getTexture(textureUrl);
            QObject::connect(texture.data(), &QObject::destroyed, this, &GeometryResource::makeStale);
        }

        // Avoid holding onto extra references
        _geometryResource.reset();
        // Make sure connection will not trigger again
        disconnect(_connection); // FIXME Should not have to do this
    }

    finishedLoading(success);
}

class GeometryReader : public QRunnable {
public:
    GeometryReader(QWeakPointer<Resource>& resource, const QUrl& url, const QVariantHash& mapping,
        const QByteArray& data) :
        _resource(resource), _url(url), _mapping(mapping), _data(data) {}
    virtual ~GeometryReader() = default;

    virtual void run() override;

private:
    QWeakPointer<Resource> _resource;
    QUrl _url;
    QVariantHash _mapping;
    QByteArray _data;
};

void GeometryReader::run() {
    auto originalPriority = QThread::currentThread()->priority();
    if (originalPriority == QThread::InheritPriority) {
        originalPriority = QThread::NormalPriority;
    }
    QThread::currentThread()->setPriority(QThread::LowPriority);
    Finally setPriorityBackToNormal([originalPriority]() {
        QThread::currentThread()->setPriority(originalPriority);
    });

    if (!_resource.data()) {
        qCWarning(modelnetworking) << "Abandoning load of" << _url << "; resource was deleted";
        return;
    }

    try {
        if (_data.isEmpty()) {
            throw QString("reply is NULL");
        }

        QString urlname = _url.path().toLower();
        if (!urlname.isEmpty() && !_url.path().isEmpty() &&
            (_url.path().toLower().endsWith(".fbx") || _url.path().toLower().endsWith(".obj"))) {
            FBXGeometry::Pointer fbxGeometry;

            if (_url.path().toLower().endsWith(".fbx")) {
                fbxGeometry.reset(readFBX(_data, _mapping, _url.path()));
                if (fbxGeometry->meshes.size() == 0 && fbxGeometry->joints.size() == 0) {
                    throw QString("empty geometry, possibly due to an unsupported FBX version");
                }
            } else if (_url.path().toLower().endsWith(".obj")) {
                fbxGeometry.reset(OBJReader().readOBJ(_data, _mapping, _url));
            } else {
                throw QString("unsupported format");
            }

            // Ensure the resource has not been deleted
            auto resource = _resource.toStrongRef();
            if (!resource) {
                qCWarning(modelnetworking) << "Abandoning load of" << _url << "; could not get strong ref";
            } else {
                QMetaObject::invokeMethod(resource.data(), "setGeometryDefinition",
                    Q_ARG(FBXGeometry::Pointer, fbxGeometry));
            }
        } else {
            throw QString("url is invalid");
        }
    } catch (const QString& error) {

        qCDebug(modelnetworking) << "Error parsing model for" << _url << ":" << error;

        auto resource = _resource.toStrongRef();
        if (resource) {
            QMetaObject::invokeMethod(resource.data(), "finishedLoading",
                Q_ARG(bool, false));
        }
    }
}

class GeometryDefinitionResource : public GeometryResource {
    Q_OBJECT
public:
    GeometryDefinitionResource(const QUrl& url, const QVariantHash& mapping, const QUrl& textureBaseUrl) :
        GeometryResource(url, resolveTextureBaseUrl(url, textureBaseUrl)), _mapping(mapping) {}

    virtual void downloadFinished(const QByteArray& data) override;

protected:
    Q_INVOKABLE void setGeometryDefinition(FBXGeometry::Pointer fbxGeometry);

private:
    QVariantHash _mapping;
};

void GeometryDefinitionResource::downloadFinished(const QByteArray& data) {
    QThreadPool::globalInstance()->start(new GeometryReader(_self, _url, _mapping, data));
}

void GeometryDefinitionResource::setGeometryDefinition(FBXGeometry::Pointer geometry) {
    // Move in joints, meshes, sitting points, and materials
    _joints = std::make_shared<NetworkJoints>(std::move(geometry->joints));
    _meshes = std::make_shared<NetworkMeshes>(std::move(geometry->meshes));
    _sittingPoints = std::make_shared<SittingPoints>(std::move(geometry->sittingPoints));

    QHash<QString, size_t> materialIDAtlas;
    for (FBXMaterial& material : geometry->materials) {
        materialIDAtlas[material.materialID] = _materials.size();
        _materials.push_back(std::make_shared<NetworkMaterial>(std::move(material), this));
    }

    // Use an intermediate object because _shapes is const.
    auto shapes = std::make_shared<NetworkShapes>();
    int meshID = 0;
    for (const NetworkMesh& mesh : *_meshes) {
        int partID = 0;
        for (const FBXMeshPart& part : mesh.parts) {
            // Construct local shapes
            shapes->emplace_back(meshID, partID, (int)materialIDAtlas[part.materialID]);
            partID++;
        }
        meshID++;
    }
    _shapes = shapes;

    finishedLoading(true);
}

ModelCache::ModelCache() {
    const qint64 GEOMETRY_DEFAULT_UNUSED_MAX_SIZE = DEFAULT_UNUSED_MAX_SIZE;
    setUnusedResourceCacheSize(GEOMETRY_DEFAULT_UNUSED_MAX_SIZE);
    setObjectName("ModelCache");
}

QSharedPointer<Resource> ModelCache::createResource(const QUrl& url, const QSharedPointer<Resource>& fallback,
                                                    bool delayLoad, const void* extra) {
    const GeometryExtra* geometryExtra = static_cast<const GeometryExtra*>(extra);

    Resource* resource = nullptr;
    if (url.path().toLower().endsWith(".fst")) {
        resource = new GeometryMappingResource(url);
    } else {
        resource = new GeometryDefinitionResource(url, geometryExtra->mapping, geometryExtra->textureBaseUrl);
    }

    return QSharedPointer<Resource>(resource, &Resource::deleter);
}

std::shared_ptr<NetworkGeometry> ModelCache::getGeometry(const QUrl& url, const QVariantHash& mapping, const QUrl& textureBaseUrl) {
    GeometryExtra geometryExtra = { mapping, textureBaseUrl };
    GeometryResource::Pointer resource = getResource(url, QUrl(), true, &geometryExtra).staticCast<GeometryResource>();
    if (resource) {
        if (resource->isLoaded()) {
            resource->resetTextures();
            // Check that no textures were freed during retrieval
            if (resource->_isStale) {
                qDebug() << "Requested model" << url << "is stale, reloading";

                // If any inlined textures are freed, the entire model must be reloaded
                resource->_isCacheable = false;
                resource.reset();
                purgeResource(url);

                return getGeometry(url, mapping, textureBaseUrl);
            }
        }
        return std::make_shared<NetworkGeometry>(resource);
    } else {
        return NetworkGeometry::Pointer();
    }
}

const QVariantMap Geometry::getTextures() const {
    QVariantMap textures;
    for (const auto& material : _materials) {
        auto map = material->getTextures();
        for (auto texture = map.cbegin(); texture != map.cend(); ++texture) {
            textures[texture.key()] = texture.value();
        }
    }

    return textures;
}

void Geometry::setTextures(const QVariantMap& textureMap) {
    if (_meshes->size() > 0) {
        for (auto& material : _materials) {
            // Check if any material textures match the textureMap
            if (std::any_of(material->_textures.cbegin(), material->_textures.cend(),
                [&textureMap](const std::vector<NetworkMaterial::Texture>::value_type& it) { return it.texture && textureMap.contains(it.name); })) { 

                if (material->isCached()) {
                    // Copy the material to avoid mutating the cached version
                    material = std::make_shared<NetworkMaterial>(*material);
                    material->_isCached = false;
                }

                // Changed textures' albedo uses alpha (transparency)
                material->setTextures(textureMap, true);
                _areTexturesLoaded = false;

                // If we only use cached textures, they should all be loaded, so we should check
                areTexturesLoaded();
            }
        }
    } else {
        qCWarning(modelnetworking) << "Ignoring setTextures(); geometry not ready";
    }
}

bool Geometry::areTexturesLoaded() const {
    if (!_areTexturesLoaded) {
        for (auto& material : _materials) {
            // Check if material textures are loaded
            if (std::any_of(material->_textures.cbegin(), material->_textures.cend(),
                [](const std::vector<NetworkMaterial::Texture>::value_type& it) { return it.texture && !it.texture->isLoaded(); })) {

                return false;
            }

            // If material textures are loaded, check the material translucency
            const auto albedoTexture = material->_textures[NetworkMaterial::MapChannel::ALBEDO_MAP];
            if (albedoTexture.texture && albedoTexture.texture->getGPUTexture()) {
                material->resetOpacityMap();
            }
        }

        _areTexturesLoaded = true;
    }
    return true;
}

std::shared_ptr<const NetworkMaterial> Geometry::getShapeMaterial(int shapeID) const {
    if ((shapeID >= 0) && (shapeID < (int)_shapes->size())) {
        int materialID = _shapes->at(shapeID).materialID;
        if ((materialID >= 0) && (materialID < (int)_materials.size())) {
            return _materials[materialID];
        }
    }
    return nullptr;
}

GeometryResource::~GeometryResource() {
    if (!_inlinedTextures.empty()) {
        _isStale = true;

        // incidentally releases all textures, but necessary for purge to work
        releaseTextures();

        // Purge inlined textures from the TextureCache when freed, as
        // they'll need to be reloaded from the model, so they are just squatting on memory
        auto textureCache = DependencyManager::get<TextureCache>();
        for (const QUrl& texture : _inlinedTextures) {
            textureCache->purgeResource(texture);
        }
    }
}

void GeometryResource::deleter() {
    if (isCacheable()) {
        // Explicitly release textures before being put in the cache
        releaseTextures();
    }

    Resource::deleter();
}

void GeometryResource::makeStale() {
    if (!isCacheable()) {
        _isStale = true;
    }
    if (!_isStale) {
        _isStale = true;
        qDebug() << sender();
        qDebug() << static_cast<Resource*>(sender())->getURL();
        // Signal is sent from base dtor, so I need to add a log to the virtual dtor to check this,
        // keying on the URL: file:///C:/Users/User/Code/hifi/build/interface/Debug/resources/meshes/being_of_light/source/Being_of_Light.fbm/BaseMesh_BeingofLight_DiffuseMap.png
        // qDebug() << static_cast<Resource*>(sender())->isCacheable();
        qCDebug(modelnetworking) << "model" << getURL() << "has gone stale";

        if (_cache) {
            _cache->purgeResource(getURL());
        }
    }
}

NetworkGeometry::NetworkGeometry(const GeometryResource::Pointer& geometry) : _resource(geometry) {
    connect(_resource.data(), &Resource::finished, this, &NetworkGeometry::resourceFinished);
    connect(_resource.data(), &Resource::onRefresh, this, &NetworkGeometry::resourceRefreshed);
    if (_resource->isLoaded()) {
        resourceFinished(!_resource->getURL().isEmpty());
    }
}

void NetworkGeometry::resourceFinished(bool success) {
    // FIXME: Model is not set up to handle a refresh
    if (_instance) {
        return;
    }
    if (success) {
        _instance = std::make_shared<Geometry>(*_resource);
    }
    emit finished(success);
}

void NetworkGeometry::resourceRefreshed() {
    // FIXME: Model is not set up to handle a refresh
    // _instance.reset();
}

const QString NetworkMaterial::NO_TEXTURE = QString();

const QString& NetworkMaterial::getTextureName(MapChannel channel) {
    if (_textures[channel].texture) {
        return _textures[channel].name;
    }
    return NO_TEXTURE;
}

QUrl NetworkMaterial::getTextureUrl(const QUrl& url, const FBXTexture& texture) {
    // If content is inline, cache it under the fbx file, not its url
    const auto baseUrl = texture.content.isEmpty() ? url : QUrl(url.url() + "/");
    return baseUrl.resolved(QUrl(texture.filename));
}

model::TextureMapPointer NetworkMaterial::fetchTextureMap(const QUrl& url, TextureType type, MapChannel channel,
    GeometryResource* geometry, const FBXTexture& fbxTexture) {
    const auto texture = DependencyManager::get<TextureCache>()->getTexture(url, type, fbxTexture.content);
    _textures[channel] = Texture { fbxTexture.name, texture };

    if (!fbxTexture.content.isEmpty()) {
        geometry->_inlinedTextures.push_back(url);
        QObject::connect(texture.data(), &QObject::destroyed, geometry, &GeometryResource::makeStale);
    }

    auto map = std::make_shared<model::TextureMap>();
    map->setTextureSource(texture->_textureSource);
    return map;
}

model::TextureMapPointer NetworkMaterial::fetchTextureMap(const QUrl& url, TextureType type, MapChannel channel) {
    const auto texture = DependencyManager::get<TextureCache>()->getTexture(url, type);
    _textures[channel].texture = texture;

    auto map = std::make_shared<model::TextureMap>();
    map->setTextureSource(texture->_textureSource);
    return map;
}

void NetworkMaterial::setTexture(TextureType type, MapChannel channel, GeometryResource* geometry, const FBXTexture& fbxTexture) {
    assert(geometry);

    const auto url = getTextureUrl(geometry->_textureBaseUrl, fbxTexture);
    auto map = fetchTextureMap(url, type, channel, geometry, fbxTexture);

    setTextureMap(channel, map);
}

void NetworkMaterial::setTexture(TextureType type, MapChannel channel, const QVariantMap& textureMap, const QString& textureName, bool useAlpha) {
    if (!textureName.isEmpty()) {
        auto url = textureMap.contains(textureName) ? textureMap[textureName].toUrl() : QUrl();
        auto map = fetchTextureMap(url, type, channel);
        if (useAlpha) {
            assert(type == ALBEDO_TEXTURE);
            map->setUseAlphaChannel(useAlpha);
        }
        setTextureMap(channel, map);
    }
}

NetworkMaterial::NetworkMaterial(const FBXMaterial&& material, GeometryResource* geometry) :
    model::Material(*material._material) {
    assert(geometry);

    // Store state
    _textures = std::vector<Texture>(MapChannel::NUM_MAP_CHANNELS);
    _originalTextures = std::make_shared<QVariantMap>(getTextures());

    // Set the textures
    if (!material.albedoTexture.filename.isEmpty()) {
        setTexture(ALBEDO_TEXTURE, MapChannel::ALBEDO_MAP, geometry, material.albedoTexture);

        // Determine transparency
        if (!material.opacityTexture.filename.isEmpty()) {
            if (material.albedoTexture.filename == material.opacityTexture.filename) {
                // TODO: Different albedo/opacity maps are not currently supported
                _originalAlpha = true; // store state
                getTextureMap(MapChannel::ALBEDO_MAP)->setUseAlphaChannel(true);
                resetOpacityMap();
            }
        }
    }

    if (!material.normalTexture.filename.isEmpty()) {
        auto type = (material.normalTexture.isBumpmap ? BUMP_TEXTURE : NORMAL_TEXTURE);
        setTexture(type, MapChannel::NORMAL_MAP, geometry, material.normalTexture);
    }

    if (!material.roughnessTexture.filename.isEmpty()) {
        setTexture(ROUGHNESS_TEXTURE, MapChannel::ROUGHNESS_MAP, geometry, material.roughnessTexture);
    } else if (!material.glossTexture.filename.isEmpty()) {
        setTexture(GLOSS_TEXTURE, MapChannel::ROUGHNESS_MAP, geometry, material.glossTexture);
    }

    if (!material.metallicTexture.filename.isEmpty()) {
        setTexture(METALLIC_TEXTURE, MapChannel::METALLIC_MAP, geometry, material.metallicTexture);
    } else if (!material.specularTexture.filename.isEmpty()) {
        setTexture(SPECULAR_TEXTURE, MapChannel::METALLIC_MAP, geometry, material.specularTexture);
    }

    if (!material.occlusionTexture.filename.isEmpty()) {
        setTexture(OCCLUSION_TEXTURE, MapChannel::OCCLUSION_MAP, geometry, material.occlusionTexture);
    }

    if (!material.emissiveTexture.filename.isEmpty()) {
        setTexture(EMISSIVE_TEXTURE, MapChannel::EMISSIVE_MAP, geometry, material.emissiveTexture);
    }

    if (!material.lightmapTexture.filename.isEmpty()) {
        setTexture(LIGHTMAP_TEXTURE, MapChannel::LIGHTMAP_MAP, geometry, material.lightmapTexture);
    }
}

void NetworkMaterial::setTextures(const QVariantMap& textureMap, bool useAlpha) {
    const auto& albedoName = getTextureName(MapChannel::ALBEDO_MAP);
    const auto& normalName = getTextureName(MapChannel::NORMAL_MAP);
    const auto& roughnessName = getTextureName(MapChannel::ROUGHNESS_MAP);
    const auto& metallicName = getTextureName(MapChannel::METALLIC_MAP);
    const auto& occlusionName = getTextureName(MapChannel::OCCLUSION_MAP);
    const auto& emissiveName = getTextureName(MapChannel::EMISSIVE_MAP);
    const auto& lightmapName = getTextureName(MapChannel::LIGHTMAP_MAP);

    setTexture(ALBEDO_TEXTURE, MapChannel::ALBEDO_MAP, textureMap, albedoName, useAlpha);
    setTexture(NORMAL_TEXTURE, MapChannel::NORMAL_MAP, textureMap, normalName);
    setTexture(ROUGHNESS_TEXTURE, MapChannel::ROUGHNESS_MAP, textureMap, roughnessName);
    // FIXME: If passing a specular map instead of a metallic how do we know?
    setTexture(METALLIC_TEXTURE, MapChannel::METALLIC_MAP, textureMap, metallicName);
    setTexture(OCCLUSION_TEXTURE, MapChannel::OCCLUSION_MAP, textureMap, occlusionName);
    setTexture(EMISSIVE_TEXTURE, MapChannel::EMISSIVE_MAP, textureMap, emissiveName);
    setTexture(LIGHTMAP_TEXTURE, MapChannel::LIGHTMAP_MAP, textureMap, lightmapName);
}

QVariantMap NetworkMaterial::getTextures() const {
    QVariantMap textures;
    for (const auto& texture : _textures) {
        if (texture.texture) {
            textures[texture.name] = texture.texture->getURL();
        }
    }
    return textures;
}

void GeometryResource::releaseTextures() {
    for (auto& material : _materials) {
        material->releaseTextures();
    }
}

void GeometryResource::resetTextures() {
    for (auto& material : _materials) {
        material->resetTextures();
    }
}

void NetworkMaterial::releaseTextures() {
    for (auto& metaTexture : _textures) {
        metaTexture.texture.reset();
    }
    releaseTextureMaps();
}

void NetworkMaterial::resetTextures() {
    setTextures(*_originalTextures, _originalAlpha);
}

#include "ModelCache.moc"
