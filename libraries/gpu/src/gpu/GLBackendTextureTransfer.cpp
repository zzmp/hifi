//
//  Created by Bradley Austin Davis on 2016/04/03
//  Copyright 2013-2016 High Fidelity, Inc.
//
//  Distributed under the Apache License, Version 2.0.
//  See the accompanying file LICENSE or http://www.apache.org/licenses/LICENSE-2.0.html
//

#include "GLBackendTextureTransfer.h"

#include "GLBackendShared.h"

#ifdef THREADED_TEXTURE_TRANSFER

#include <gl/OffscreenGLCanvas.h>
#include <gl/QOpenGLContextWrapper.h>


#endif

using namespace gpu;

GLTextureTransferHelper::GLTextureTransferHelper() {
#ifdef THREADED_TEXTURE_TRANSFER
    _canvas = QSharedPointer<OffscreenGLCanvas>(new OffscreenGLCanvas(), &QObject::deleteLater);
    _canvas->create(QOpenGLContextWrapper::currentContext());
    if (!_canvas->makeCurrent()) {
        qFatal("Unable to create texture transfer context");
    }
    _canvas->doneCurrent();
    initialize(true, QThread::LowPriority);
    _canvas->moveToThreadWithContext(_thread);

    // Clean shutdown on UNIX, otherwise _canvas is freed early
    connect(qApp, &QCoreApplication::aboutToQuit, [&] { terminate(); });
#endif
}

GLTextureTransferHelper::~GLTextureTransferHelper() {
#ifdef THREADED_TEXTURE_TRANSFER
    if (isStillRunning()) {
        terminate();
    }
#endif
}

void GLTextureTransferHelper::transferTexture(const gpu::TexturePointer& texture) {
    assert(texture);

#ifdef THREADED_TEXTURE_TRANSFER
    GLBackend::GLTexture* object = Backend::getGPUObject<GLBackend::GLTexture>(*texture);
    TextureTransferPackage package{ texture, 0};
    object->setSyncState(GLBackend::GLTexture::Pending);
    queueItem(package);
#else
    transferTextureSynchronous(texture);
#endif
}

void GLTextureTransferHelper::setup() {
#ifdef THREADED_TEXTURE_TRANSFER
    _canvas->makeCurrent();
#endif
}

void GLTextureTransferHelper::shutdown() {
#ifdef THREADED_TEXTURE_TRANSFER
    _canvas->doneCurrent();
    _canvas->moveToThreadWithContext(qApp->thread());
    _canvas.reset();
#endif
}


bool GLTextureTransferHelper::processQueueItems(const Queue& messages) {
    for (auto package : messages) {
        transferTextureSynchronous(package.texture.lock());
    }
    return true;
}

void transferTextureSynchronous(const gpu::TexturePointer& texture) {
    // Make sure the texture exists
    if (!texture) {
        return;
    }

    GLBackend::GLTexture* object = Backend::getGPUObject<GLBackend::GLTexture>(*texture);
    object->createTexture();

    object->transfer();

    object->updateSize();

    glBindTexture(object->_target, 0);
    auto writeSync = glFenceSync(GL_SYNC_GPU_COMMANDS_COMPLETE, 0);
    glClientWaitSync(writeSync, GL_SYNC_FLUSH_COMMANDS_BIT, GL_TIMEOUT_IGNORED);
    glDeleteSync(writeSync);

    object->_contentStamp = texture->getDataStamp();
    object->setSyncState(GLBackend::GLTexture::Transferred);

    texture->notifyTransferred();
}
