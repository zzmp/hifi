//
//  ModelCache.h
//  libraries/model-networking
//
//  Created by Zach Pomerantz on 3/15/16.
//  Copyright 2016 High Fidelity, Inc.
//
//  Distributed under the Apache License, Version 2.0.
//  See the accompanying file LICENSE or http://www.apache.org/licenses/LICENSE-2.0.html
//

#ifndef hifi_ModelCache_h
#define hifi_ModelCache_h

#include <DependencyManager.h>
#include <ResourceCache.h>

#include <model/Material.h>
#include <model/Asset.h>

#include "FBXReader.h"
#include "TextureCache.h"

using NetworkJoint = FBXJoint;
using NetworkJoints = FBXJoints;

using NetworkMesh = FBXMesh;
using NetworkMeshes = FBXMeshes;

class NetworkMaterial;
class NetworkTexture;
class NetworkGeometry;
class GeometryMappingResource;

/// Stores cached model geometries.
class ModelCache : public ResourceCache, public Dependency {
    Q_OBJECT
    SINGLETON_DEPENDENCY

public:
    /// Loads a model geometry from the specified URL.
    std::shared_ptr<NetworkGeometry> getGeometry(const QUrl& url,
        const QVariantHash& mapping = QVariantHash(), const QUrl& textureBaseUrl = QUrl());

protected:
    friend class GeometryMappingResource;

    virtual QSharedPointer<Resource> createResource(const QUrl& url,
        const QSharedPointer<Resource>& fallback, bool delayLoad, const void* extra);

private:
    ModelCache();
    virtual ~ModelCache() = default;
};

/// A material loaded from the network.
/// Materials include rendering hints (in the key) and textures.
class NetworkMaterial : public model::Material {
public:
    NetworkMaterial(const FBXMaterial&& material, const QUrl& textureBaseUrl);

    QVariantMap getTextures() const;

protected:
    friend class Geometry;

    using MapChannel = model::Material::MapChannel;

    void setTextures(const QVariantMap& textureMap);

    static const QString NO_TEXTURE;
    const QString& getTextureName(MapChannel channel);

    // Textures should not be held while cached; that is for the TextureCache, not the ModelCache.
    void releaseTextures(); // release textures when caching material
    void resetTextures(); // reset textures when retrieving material from the cache

    // Materials can be mutated, but the cached version should never be changed.
    const bool& isCached() const { return _isCached; }

    class Texture {
    public:
        QString name;
        QSharedPointer<NetworkTexture> texture;
    };
    std::vector<Texture> _textures;

private:
    // Helpers for the ctors
    QUrl getTextureUrl(const QUrl& baseUrl, const FBXTexture& fbxTexture);
    model::TextureMapPointer fetchTextureMap(const QUrl& baseUrl, const FBXTexture& fbxTexture,
        TextureType type, MapChannel channel);
    model::TextureMapPointer fetchTextureMap(const QUrl& url, TextureType type, MapChannel channel);

    // State to keep between all instances
    class State {
    public:
        QVariantMap _originalTextures;
        Transform _albedoTransform;
        Transform _lightmapTransform;
        vec2 _lightmapParams;
    };
    std::shared_ptr<State> _state;

    bool _isCached { true };
};


class Geometry {
public:
    using Pointer = std::shared_ptr<Geometry>;

    const NetworkJoints& getJoints() const { return *_joints; }
    const SittingPoints& getSittingPoints() const { return *_sittingPoints; }
    const NetworkMeshes& getMeshes() const { return *_meshes; }

    std::shared_ptr<const NetworkMaterial> getShapeMaterial(int shapeID) const;

    const QVariantMap getTextures() const;
    void setTextures(const QVariantMap& textureMap);

    virtual bool areTexturesLoaded() const;

protected:
    friend class GeometryMappingResource;

    using NetworkMaterials = std::vector<std::shared_ptr<NetworkMaterial>>;

    // A map between meshes, parts, and materials.
    class NetworkShape {
    public:
        NetworkShape(int mesh, int part, int material) : meshID { mesh }, partID { part }, materialID { material } {}

        int meshID { -1 };
        int partID { -1 };
        int materialID { -1 };
    };
    using NetworkShapes = std::vector<NetworkShape>;


    // Shared across all geometries, constant throughout lifetime
    std::shared_ptr<const NetworkJoints> _joints;
    std::shared_ptr<const SittingPoints> _sittingPoints;
    std::shared_ptr<const NetworkMeshes> _meshes;
    std::shared_ptr<const NetworkShapes> _shapes;

    // Copied to each geometry, mutable throughout lifetime via setTextures
    NetworkMaterials _materials;

    // Textures should not be held while cached; that is for the TextureCache, not the ModelCache.
    void releaseTextures();
    void resetTextures();

private:
    mutable bool _areTexturesLoaded { false };
};

/// A geometry loaded from the network.
class GeometryResource : public Resource, public Geometry {
public:
    using Pointer = QSharedPointer<GeometryResource>;

    GeometryResource(const QUrl& url, const QUrl& textureBaseUrl = QUrl()) :
        Resource(url), _textureBaseUrl(textureBaseUrl) {}

    virtual bool areTexturesLoaded() const { return isLoaded() && Geometry::areTexturesLoaded(); }

    virtual void deleter() override;

protected:
    friend class ModelCache;
    friend class GeometryMappingResource;

    QUrl _textureBaseUrl;

    virtual bool isCacheable() const override { return _loaded && _isCacheable; }
    bool _isCacheable { true };
};

class NetworkGeometry : public QObject {
    Q_OBJECT
public:
    using Pointer = std::shared_ptr<NetworkGeometry>;

    NetworkGeometry() = delete;
    NetworkGeometry(const GeometryResource::Pointer& geometry);

    const QUrl& getURL() { return _resource->getURL(); }

    /// Returns the geometry, if it is loaded (must be checked!)
    const Geometry::Pointer& getGeometry() { return _instance; }

signals:
    /// Emitted when the NetworkGeometry loads (or fails to)
    void finished(bool success);

private slots:
    void resourceFinished(bool success);
    void resourceRefreshed();

private:
    GeometryResource::Pointer _resource;
    Geometry::Pointer _instance { nullptr };
};

#endif // hifi_ModelCache_h
