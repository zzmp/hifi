//
//  Context.cpp
//  render/src/render
//
//  Created by Zach Pomerantz on 1/6/2015.
//  Copyright 2015 High Fidelity, Inc.
//
//  Distributed under the Apache License, Version 2.0.
//  See the accompanying file LICENSE or http://www.apache.org/licenses/LICENSE-2.0.html
//

#include "Context.h"

using namespace render;

RenderContext::RenderContext(int drawStatus, bool drawHitEffect)
    : _args{ nullptr },
    _drawStatus{ drawStatus }, _drawHitEffect{ drawHitEffect } {
}

void RenderContext::setOptions(bool occlusion, bool fxaa, bool showOwned, bool shadowMap) {
    _occlusionStatus = occlusion;
    _fxaaStatus = fxaa;
    _shadowMapStatus = shadowMap;

    if (showOwned) {
        _drawStatus |= render::showNetworkStatusFlag;
    }
};

