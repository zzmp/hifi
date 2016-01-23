//
//  DrawTask.h
//  render/src/render
//
//  Created by Sam Gateau on 5/21/15.
//  Copyright 20154 High Fidelity, Inc.
//
//  Distributed under the Apache License, Version 2.0.
//  See the accompanying file LICENSE or http://www.apache.org/licenses/LICENSE-2.0.html
//

#ifndef hifi_render_DrawTask_h
#define hifi_render_DrawTask_h

#include "Engine.h"
#include "ShapePipeline.h"
#include "gpu/Batch.h"


namespace render {

using CullFunctor = std::function<bool(const RenderArgs*, const AABox&)>;

void cullItems(const RenderContextPointer& renderContext, const CullFunctor& cullFunctor, RenderDetails::Item& details,
               const ItemIDsBounds& inItems, ItemIDsBounds& outItems);
void depthSortItems(const SceneContextPointer& sceneContext, const RenderContextPointer& renderContext, bool frontToBack, const ItemIDsBounds& inItems, ItemIDsBounds& outItems);
void renderItems(const SceneContextPointer& sceneContext, const RenderContextPointer& renderContext, const ItemIDsBounds& inItems);
void renderShapes(const SceneContextPointer& sceneContext, const RenderContextPointer& renderContext, const ShapePlumberPointer& shapeContext, const ItemIDsBounds& inItems, int maxDrawnItems = -1);

class FetchItems {
public:
    typedef std::function<void (const RenderContextPointer& context, int count)> ProbeNumItems;
    FetchItems() {}
    FetchItems(const ProbeNumItems& probe): _probeNumItems(probe) {}
    FetchItems(const ItemFilter& filter, const ProbeNumItems& probe): _filter(filter), _probeNumItems(probe) {}

    ItemFilter _filter = ItemFilter::Builder::opaqueShape().withoutLayered();
    ProbeNumItems _probeNumItems;

    void run(const SceneContextPointer& sceneContext, const RenderContextPointer& renderContext, ItemIDsBounds& outItems);
    using JobModel = Job::ModelO<FetchItems, ItemIDsBounds>;
};

template<RenderDetails::Type T = RenderDetails::Type::OTHER_ITEM>
class CullItems {
public:
    CullItems(CullFunctor cullFunctor) : _cullFunctor{ cullFunctor } {}

    void run(const SceneContextPointer& sceneContext, const RenderContextPointer& renderContext, const ItemIDsBounds& inItems, ItemIDsBounds& outItems) {
        const auto& args = renderContext->getArgs();
        auto& details = args->_details.edit(T);
        outItems.clear();
        outItems.reserve(inItems.size());
        render::cullItems(renderContext, _cullFunctor, details, inItems, outItems);
    }

    using JobModel = Job::ModelIO<CullItems<T>, ItemIDsBounds, ItemIDsBounds>;

protected:
    CullFunctor _cullFunctor;
};

class DepthSortItems {
public:
    bool _frontToBack;
    DepthSortItems(bool frontToBack = true) : _frontToBack(frontToBack) {}

    void run(const SceneContextPointer& sceneContext, const RenderContextPointer& renderContext, const ItemIDsBounds& inItems, ItemIDsBounds& outItems);
    using JobModel = Job::ModelIO<DepthSortItems, ItemIDsBounds, ItemIDsBounds>;
};

class DrawLight {
public:
    DrawLight(CullFunctor cullFunctor) : _cullFunctor{ cullFunctor } {}
    void run(const SceneContextPointer& sceneContext, const RenderContextPointer& renderContext);
    using JobModel = Job::Model<DrawLight>;

protected:
    CullFunctor _cullFunctor;
};

class PipelineSortShapes {
public:
    void run(const SceneContextPointer& sceneContext, const RenderContextPointer& renderContext, const ItemIDsBounds& inItems, ShapesIDsBounds& outShapes);
    using JobModel = Job::ModelIO<PipelineSortShapes, ItemIDsBounds, ShapesIDsBounds>;
};

class DepthSortShapes {
public:
    bool _frontToBack;
    DepthSortShapes(bool frontToBack = true) : _frontToBack(frontToBack) {}

    void run(const SceneContextPointer& sceneContext, const RenderContextPointer& renderContext, const ShapesIDsBounds& inShapes, ShapesIDsBounds& outShapes);
    using JobModel = Job::ModelIO<DepthSortShapes, ShapesIDsBounds, ShapesIDsBounds>;
};

}

#endif // hifi_render_DrawTask_h
