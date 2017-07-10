//
//  RenderablePolyVoxEntityItem.h
//  libraries/entities-renderer/src/
//
//  Created by Seth Alves on 5/19/15.
//  Copyright 2015 High Fidelity, Inc.
//
//  Distributed under the Apache License, Version 2.0.
//  See the accompanying file LICENSE or http://www.apache.org/licenses/LICENSE-2.0.html
//

#ifndef hifi_RenderablePolyVoxEntityItem_h
#define hifi_RenderablePolyVoxEntityItem_h

#include <atomic>

#include <QSemaphore>

#include <PolyVoxCore/SimpleVolume.h>
#include <PolyVoxCore/Raycast.h>

#include <gpu/Context.h>
#include <model/Forward.h>
#include <TextureCache.h>
#include <PolyVoxEntityItem.h>

#include "RenderableEntityItem.h"

class PolyVoxPayload {
public:
    PolyVoxPayload(EntityItemPointer owner) : _owner(owner), _bounds(AABox()) { }
    typedef render::Payload<PolyVoxPayload> Payload;
    typedef Payload::DataPointer Pointer;

    EntityItemPointer _owner;
    AABox _bounds;
};

namespace render {
   template <> const ItemKey payloadGetKey(const PolyVoxPayload::Pointer& payload);
   template <> const Item::Bound payloadGetBound(const PolyVoxPayload::Pointer& payload);
   template <> void payloadRender(const PolyVoxPayload::Pointer& payload, RenderArgs* args);
}


class RenderablePolyVoxEntityItem : public PolyVoxEntityItem, public RenderableEntityInterface {
public:
    static EntityItemPointer factory(const EntityItemID& entityID, const EntityItemProperties& properties);
    RenderablePolyVoxEntityItem(const EntityItemID& entityItemID);

    virtual ~RenderablePolyVoxEntityItem();

    RenderableEntityInterface* getRenderableInterface() override { return this; }

    void initializePolyVox();

    virtual void somethingChangedNotification() override {
        // This gets called from EnityItem::readEntityDataFromBuffer every time a packet describing
        // this entity comes from the entity-server.  It gets called even if nothing has actually changed
        // (see the comment in EntityItem.cpp).  If that gets fixed, this could be used to know if we
        // need to redo the voxel data.
        // _needsModelReload = true;
    }

    virtual uint8_t getVoxel(int x, int y, int z) override;
    virtual bool setVoxel(int x, int y, int z, uint8_t toValue) override;

    int getOnCount() const override { return _onCount; }

    void render(RenderArgs* args) override;
    virtual bool supportsDetailedRayIntersection() const override { return true; }
    virtual bool findDetailedRayIntersection(const glm::vec3& origin, const glm::vec3& direction,
                        bool& keepSearching, OctreeElementPointer& element, float& distance,
                        BoxFace& face, glm::vec3& surfaceNormal,
                        void** intersectedObject, bool precisionPicking) const override;

    virtual void setVoxelData(QByteArray voxelData) override;
    virtual void setVoxelVolumeSize(glm::vec3 voxelVolumeSize) override;
    virtual void setVoxelSurfaceStyle(PolyVoxSurfaceStyle voxelSurfaceStyle) override;

    glm::vec3 getSurfacePositionAdjustment() const;
    glm::mat4 voxelToWorldMatrix() const;
    glm::mat4 worldToVoxelMatrix() const;
    glm::mat4 voxelToLocalMatrix() const;
    glm::mat4 localToVoxelMatrix() const;

    virtual ShapeType getShapeType() const override;
    virtual bool shouldBePhysical() const override { return !isDead(); }
    virtual bool isReadyToComputeShape() override;
    virtual void computeShapeInfo(ShapeInfo& info) override;

    virtual glm::vec3 voxelCoordsToWorldCoords(glm::vec3& voxelCoords) const override;
    virtual glm::vec3 worldCoordsToVoxelCoords(glm::vec3& worldCoords) const override;
    virtual glm::vec3 voxelCoordsToLocalCoords(glm::vec3& voxelCoords) const override;
    virtual glm::vec3 localCoordsToVoxelCoords(glm::vec3& localCoords) const override;

    // coords are in voxel-volume space
    virtual bool setSphereInVolume(glm::vec3 center, float radius, uint8_t toValue) override;
    virtual bool setVoxelInVolume(glm::vec3 position, uint8_t toValue) override;

    // coords are in world-space
    virtual bool setSphere(glm::vec3 center, float radius, uint8_t toValue) override;
    virtual bool setCapsule(glm::vec3 startWorldCoords, glm::vec3 endWorldCoords,
                            float radiusWorldCoords, uint8_t toValue) override;
    virtual bool setAll(uint8_t toValue) override;
    virtual bool setCuboid(const glm::vec3& lowPosition, const glm::vec3& cuboidSize, int toValue) override;

    virtual void setXTextureURL(const QString& xTextureURL) override;
    virtual void setYTextureURL(const QString& yTextureURL) override;
    virtual void setZTextureURL(const QString& zTextureURL) override;

    virtual bool addToScene(const EntityItemPointer& self,
                            const render::ScenePointer& scene,
                            render::Transaction& transaction) override;
    virtual void removeFromScene(const EntityItemPointer& self,
                                 const render::ScenePointer& scene,
                                 render::Transaction& transaction) override;

    virtual void setXNNeighborID(const EntityItemID& xNNeighborID) override;
    virtual void setYNNeighborID(const EntityItemID& yNNeighborID) override;
    virtual void setZNNeighborID(const EntityItemID& zNNeighborID) override;

    virtual void setXPNeighborID(const EntityItemID& xPNeighborID) override;
    virtual void setYPNeighborID(const EntityItemID& yPNeighborID) override;
    virtual void setZPNeighborID(const EntityItemID& zPNeighborID) override;

    std::shared_ptr<RenderablePolyVoxEntityItem> getXNNeighbor();
    std::shared_ptr<RenderablePolyVoxEntityItem> getYNNeighbor();
    std::shared_ptr<RenderablePolyVoxEntityItem> getZNNeighbor();
    std::shared_ptr<RenderablePolyVoxEntityItem> getXPNeighbor();
    std::shared_ptr<RenderablePolyVoxEntityItem> getYPNeighbor();
    std::shared_ptr<RenderablePolyVoxEntityItem> getZPNeighbor();

    virtual void updateRegistrationPoint(const glm::vec3& value) override;

    void setVoxelsFromData(QByteArray uncompressedData, quint16 voxelXSize, quint16 voxelYSize, quint16 voxelZSize);
    void forEachVoxelValue(quint16 voxelXSize, quint16 voxelYSize, quint16 voxelZSize,
                           std::function<void(int, int, int, uint8_t)> thunk);
    QByteArray volDataToArray(quint16 voxelXSize, quint16 voxelYSize, quint16 voxelZSize) const;

    void setMesh(model::MeshPointer mesh);
    void setCollisionPoints(ShapeInfo::PointCollection points, AABox box);
    PolyVox::SimpleVolume<uint8_t>* getVolData() { return _volData; }

    uint8_t getVoxelInternal(int x, int y, int z) const;
    bool setVoxelInternal(int x, int y, int z, uint8_t toValue);

    void setVolDataDirty() { withWriteLock([&] { _volDataDirty = true; _meshReady = false; }); }

    // Transparent polyvox didn't seem to be working so disable for now
    bool isTransparent() override { return false; }

    bool getMeshes(MeshProxyList& result) override;

protected:
    virtual void locationChanged(bool tellPhysics = true) override;

private:
    // The PolyVoxEntityItem class has _voxelData which contains dimensions and compressed voxel data.  The dimensions
    // may not match _voxelVolumeSize.

    model::MeshPointer _mesh;
    gpu::Stream::FormatPointer _vertexFormat;
    bool _meshDirty { true }; // does collision-shape need to be recomputed?
    bool _meshReady { false };

    NetworkTexturePointer _xTexture;
    NetworkTexturePointer _yTexture;
    NetworkTexturePointer _zTexture;

    const int MATERIAL_GPU_SLOT = 3;
    render::ItemID _myItem{ render::Item::INVALID_ITEM_ID };
    static gpu::PipelinePointer _pipeline;
    static gpu::PipelinePointer _wireframePipeline;

    ShapeInfo _shapeInfo;

    PolyVox::SimpleVolume<uint8_t>* _volData = nullptr;
    bool _volDataDirty = false; // does recomputeMesh need to be called?
    int _onCount; // how many non-zero voxels are in _volData

    bool _neighborsNeedUpdate { false };

    bool updateOnCount(int x, int y, int z, uint8_t toValue);
    PolyVox::RaycastResult doRayCast(glm::vec4 originInVoxel, glm::vec4 farInVoxel, glm::vec4& result) const;

    // these are run off the main thread
    void decompressVolumeData();
    void compressVolumeDataAndSendEditPacket();
    virtual void recomputeMesh() override; // recompute mesh
    void computeShapeInfoWorker();

    // these are cached lookups of _xNNeighborID, _yNNeighborID, _zNNeighborID, _xPNeighborID, _yPNeighborID, _zPNeighborID
    EntityItemWeakPointer _xNNeighbor; // neighbor found by going along negative X axis
    EntityItemWeakPointer _yNNeighbor;
    EntityItemWeakPointer _zNNeighbor;
    EntityItemWeakPointer _xPNeighbor; // neighbor found by going along positive X axis
    EntityItemWeakPointer _yPNeighbor;
    EntityItemWeakPointer _zPNeighbor;
    void cacheNeighbors();
    void copyUpperEdgesFromNeighbors();
    void bonkNeighbors();
    bool updateDependents();
};

bool inUserBounds(const PolyVox::SimpleVolume<uint8_t>* vol, PolyVoxEntityItem::PolyVoxSurfaceStyle surfaceStyle,
                  int x, int y, int z);

#endif // hifi_RenderablePolyVoxEntityItem_h
