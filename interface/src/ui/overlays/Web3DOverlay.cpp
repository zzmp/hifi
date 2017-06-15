//
//  Web3DOverlay.cpp
//
//  Created by Clement on 7/1/14.
//  Modified and renamed by Zander Otavka on 8/4/15
//  Copyright 2014 High Fidelity, Inc.
//
//  Distributed under the Apache License, Version 2.0.
//  See the accompanying file LICENSE or http://www.apache.org/licenses/LICENSE-2.0.html
//

#include "Web3DOverlay.h"

#include <Application.h>

#include <QQuickWindow>
#include <QtGui/QOpenGLContext>
#include <QtQuick/QQuickItem>
#include <QtQml/QQmlContext>
#include <QtQml/QQmlEngine>

#include <AbstractViewStateInterface.h>
#include <gpu/Batch.h>
#include <DependencyManager.h>
#include <GeometryCache.h>
#include <GeometryUtil.h>
#include <scripting/HMDScriptingInterface.h>
#include <gl/OffscreenQmlSurface.h>
#include <PathUtils.h>
#include <RegisteredMetaTypes.h>
#include <TabletScriptingInterface.h>
#include <TextureCache.h>
#include <UsersScriptingInterface.h>
#include <UserActivityLoggerScriptingInterface.h>
#include <AbstractViewStateInterface.h>
#include <gl/OffscreenQmlSurface.h>
#include <gl/OffscreenQmlSurfaceCache.h>
#include <AddressManager.h>
#include "scripting/AccountScriptingInterface.h"
#include "scripting/HMDScriptingInterface.h"
#include "scripting/AssetMappingsScriptingInterface.h"
#include <Preferences.h>
#include <ScriptEngines.h>
#include "FileDialogHelper.h"
#include "avatar/AvatarManager.h"
#include "AudioClient.h"
#include "LODManager.h"
#include "ui/OctreeStatsProvider.h"
#include "ui/DomainConnectionModel.h"
#include "ui/AvatarInputs.h"
#include "avatar/AvatarManager.h"
#include "scripting/GlobalServicesScriptingInterface.h"
#include "ui/Snapshot.h"

static const float DPI = 30.47f;
static const float INCHES_TO_METERS = 1.0f / 39.3701f;
static const float METERS_TO_INCHES = 39.3701f;
static const float OPAQUE_ALPHA_THRESHOLD = 0.99f;

const QString Web3DOverlay::TYPE = "web3d";
const QString Web3DOverlay::QML = "Web3DOverlay.qml";
Web3DOverlay::Web3DOverlay() : _dpi(DPI) {
    _touchDevice.setCapabilities(QTouchDevice::Position);
    _touchDevice.setType(QTouchDevice::TouchScreen);
    _touchDevice.setName("RenderableWebEntityItemTouchDevice");
    _touchDevice.setMaximumTouchPoints(4);

    _geometryId = DependencyManager::get<GeometryCache>()->allocateID();
}

Web3DOverlay::Web3DOverlay(const Web3DOverlay* Web3DOverlay) :
    Billboard3DOverlay(Web3DOverlay),
    _url(Web3DOverlay->_url),
    _scriptURL(Web3DOverlay->_scriptURL),
    _dpi(Web3DOverlay->_dpi),
    _resolution(Web3DOverlay->_resolution),
    _showKeyboardFocusHighlight(Web3DOverlay->_showKeyboardFocusHighlight)
{
    _geometryId = DependencyManager::get<GeometryCache>()->allocateID();
}

Web3DOverlay::~Web3DOverlay() {
    if (_webSurface) {
        QQuickItem* rootItem = _webSurface->getRootItem();

        if (rootItem && rootItem->objectName() == "tabletRoot") {
            auto tabletScriptingInterface = DependencyManager::get<TabletScriptingInterface>();
            tabletScriptingInterface->setQmlTabletRoot("com.highfidelity.interface.tablet.system", nullptr, nullptr);
        }

        // Fix for crash in QtWebEngineCore when rapidly switching domains
        // Call stop on the QWebEngineView before destroying OffscreenQMLSurface.
        if (rootItem) {
            QObject* obj = rootItem->findChild<QObject*>("webEngineView");
            if (obj) {
                // stop loading
                QMetaObject::invokeMethod(obj, "stop");
            }
        }

        _webSurface->pause();
        _webSurface->disconnect(_connection);

        QObject::disconnect(_mousePressConnection);
        _mousePressConnection = QMetaObject::Connection();
        QObject::disconnect(_mouseReleaseConnection);
        _mouseReleaseConnection = QMetaObject::Connection();
        QObject::disconnect(_mouseMoveConnection);
        _mouseMoveConnection = QMetaObject::Connection();
        QObject::disconnect(_hoverLeaveConnection);
        _hoverLeaveConnection = QMetaObject::Connection();

        QObject::disconnect(_emitScriptEventConnection);
        _emitScriptEventConnection = QMetaObject::Connection();
        QObject::disconnect(_webEventReceivedConnection);
        _webEventReceivedConnection = QMetaObject::Connection();

        // The lifetime of the QML surface MUST be managed by the main thread
        // Additionally, we MUST use local variables copied by value, rather than
        // member variables, since they would implicitly refer to a this that
        // is no longer valid
        auto webSurface = _webSurface;
        AbstractViewStateInterface::instance()->postLambdaEvent([webSurface] {
            DependencyManager::get<OffscreenQmlSurfaceCache>()->release(QML, webSurface);
        });
        _webSurface.reset();
    }
    auto geometryCache = DependencyManager::get<GeometryCache>();
    if (geometryCache) {
        geometryCache->releaseID(_geometryId);
    }
}

void Web3DOverlay::update(float deltatime) {
    if (_webSurface) {
        // update globalPosition
        _webSurface->getSurfaceContext()->setContextProperty("globalPosition", vec3toVariant(getPosition()));
    }
}

QString Web3DOverlay::pickURL() {
    QUrl sourceUrl(_url);
    if (sourceUrl.scheme() == "http" || sourceUrl.scheme() == "https" ||
        _url.toLower().endsWith(".htm") || _url.toLower().endsWith(".html")) {

        _webSurface->setBaseUrl(QUrl::fromLocalFile(PathUtils::resourcesPath() + "/qml/"));
        return "Web3DOverlay.qml";
    } else {
        return QUrl::fromLocalFile(PathUtils::resourcesPath()).toString() + "/" + _url;
    }
}


void Web3DOverlay::loadSourceURL() {

    QUrl sourceUrl(_url);
    if (sourceUrl.scheme() == "http" || sourceUrl.scheme() == "https" ||
        _url.toLower().endsWith(".htm") || _url.toLower().endsWith(".html")) {

        _webSurface->setBaseUrl(QUrl::fromLocalFile(PathUtils::resourcesPath() + "/qml/"));
        _webSurface->load("Web3DOverlay.qml");
        _webSurface->resume();
        _webSurface->getRootItem()->setProperty("url", _url);
        _webSurface->getRootItem()->setProperty("scriptURL", _scriptURL);

    } else {
        _webSurface->setBaseUrl(QUrl::fromLocalFile(PathUtils::resourcesPath()));
        _webSurface->load(_url, [&](QQmlContext* context, QObject* obj) {});
        _webSurface->resume();

        _webSurface->getSurfaceContext()->setContextProperty("Users", DependencyManager::get<UsersScriptingInterface>().data());
        _webSurface->getSurfaceContext()->setContextProperty("HMD", DependencyManager::get<HMDScriptingInterface>().data());
        _webSurface->getSurfaceContext()->setContextProperty("UserActivityLogger", DependencyManager::get<UserActivityLoggerScriptingInterface>().data());
        _webSurface->getSurfaceContext()->setContextProperty("Preferences", DependencyManager::get<Preferences>().data());
        _webSurface->getSurfaceContext()->setContextProperty("Vec3", new Vec3());
        _webSurface->getSurfaceContext()->setContextProperty("Quat", new Quat());
        _webSurface->getSurfaceContext()->setContextProperty("MyAvatar", DependencyManager::get<AvatarManager>()->getMyAvatar().get());
        _webSurface->getSurfaceContext()->setContextProperty("Entities", DependencyManager::get<EntityScriptingInterface>().data());
        _webSurface->getSurfaceContext()->setContextProperty("Snapshot", DependencyManager::get<Snapshot>().data());

        if (_webSurface->getRootItem() && _webSurface->getRootItem()->objectName() == "tabletRoot") {
            auto tabletScriptingInterface = DependencyManager::get<TabletScriptingInterface>();
            auto flags = tabletScriptingInterface->getFlags();
            _webSurface->getSurfaceContext()->setContextProperty("offscreenFlags", flags);
            _webSurface->getSurfaceContext()->setContextProperty("AddressManager", DependencyManager::get<AddressManager>().data());
            _webSurface->getSurfaceContext()->setContextProperty("Account", AccountScriptingInterface::getInstance());
            _webSurface->getSurfaceContext()->setContextProperty("Audio", DependencyManager::get<AudioScriptingInterface>().data());
            _webSurface->getSurfaceContext()->setContextProperty("AudioStats", DependencyManager::get<AudioClient>()->getStats().data());
            _webSurface->getSurfaceContext()->setContextProperty("HMD", DependencyManager::get<HMDScriptingInterface>().data());
            _webSurface->getSurfaceContext()->setContextProperty("fileDialogHelper", new FileDialogHelper());
            _webSurface->getSurfaceContext()->setContextProperty("MyAvatar", DependencyManager::get<AvatarManager>()->getMyAvatar().get());
            _webSurface->getSurfaceContext()->setContextProperty("ScriptDiscoveryService", DependencyManager::get<ScriptEngines>().data());
            _webSurface->getSurfaceContext()->setContextProperty("Tablet", DependencyManager::get<TabletScriptingInterface>().data());
            _webSurface->getSurfaceContext()->setContextProperty("Assets", DependencyManager::get<AssetMappingsScriptingInterface>().data());
            _webSurface->getSurfaceContext()->setContextProperty("LODManager", DependencyManager::get<LODManager>().data());
            _webSurface->getSurfaceContext()->setContextProperty("OctreeStats", DependencyManager::get<OctreeStatsProvider>().data());
            _webSurface->getSurfaceContext()->setContextProperty("DCModel", DependencyManager::get<DomainConnectionModel>().data());
            _webSurface->getSurfaceContext()->setContextProperty("AvatarInputs", AvatarInputs::getInstance());
            _webSurface->getSurfaceContext()->setContextProperty("GlobalServices", GlobalServicesScriptingInterface::getInstance());
            _webSurface->getSurfaceContext()->setContextProperty("AvatarList", DependencyManager::get<AvatarManager>().data());
            _webSurface->getSurfaceContext()->setContextProperty("DialogsManager", DialogsManagerScriptingInterface::getInstance());

            _webSurface->getSurfaceContext()->setContextProperty("pathToFonts", "../../");
            tabletScriptingInterface->setQmlTabletRoot("com.highfidelity.interface.tablet.system", _webSurface->getRootItem(), _webSurface.data());

            // mark the TabletProxy object as cpp ownership.
            QObject* tablet = tabletScriptingInterface->getTablet("com.highfidelity.interface.tablet.system");
            _webSurface->getSurfaceContext()->engine()->setObjectOwnership(tablet, QQmlEngine::CppOwnership);

            // Override min fps for tablet UI, for silky smooth scrolling
            setMaxFPS(90);
        }
    }
    _webSurface->getSurfaceContext()->setContextProperty("globalPosition", vec3toVariant(getPosition()));
}

void Web3DOverlay::setMaxFPS(uint8_t maxFPS) {
    _desiredMaxFPS = maxFPS;
    if (_webSurface) {
        _webSurface->setMaxFps(_desiredMaxFPS);
        _currentMaxFPS = _desiredMaxFPS;
    }
}

void Web3DOverlay::render(RenderArgs* args) {
    QOpenGLContext * currentContext = QOpenGLContext::currentContext();
    QSurface * currentSurface = currentContext->surface();
    if (!_webSurface) {
        _webSurface = DependencyManager::get<OffscreenQmlSurfaceCache>()->acquire(pickURL());
        // FIXME, the max FPS could be better managed by being dynamic (based on the number of current surfaces
        // and the current rendering load)
        if (_currentMaxFPS != _desiredMaxFPS) {
            setMaxFPS(_desiredMaxFPS);
        }
        loadSourceURL();
        _webSurface->resume();
        _webSurface->resize(QSize(_resolution.x, _resolution.y));
        _webSurface->getRootItem()->setProperty("url", _url);
        _webSurface->getRootItem()->setProperty("scriptURL", _scriptURL);
        currentContext->makeCurrent(currentSurface);

        auto selfOverlayID = getOverlayID();
        std::weak_ptr<Web3DOverlay> weakSelf = std::dynamic_pointer_cast<Web3DOverlay>(qApp->getOverlays().getOverlay(selfOverlayID));
        auto forwardPointerEvent = [=](OverlayID overlayID, const PointerEvent& event) {
            auto self = weakSelf.lock();
            if (self && overlayID == selfOverlayID) {
                self->handlePointerEvent(event);
            }
        };

        _mousePressConnection = connect(&(qApp->getOverlays()), &Overlays::mousePressOnOverlay, this, forwardPointerEvent, Qt::DirectConnection);
        _mouseReleaseConnection = connect(&(qApp->getOverlays()), &Overlays::mouseReleaseOnOverlay, this, forwardPointerEvent, Qt::DirectConnection);
        _mouseMoveConnection = connect(&(qApp->getOverlays()), &Overlays::mouseMoveOnOverlay, this, forwardPointerEvent, Qt::DirectConnection);
        _hoverLeaveConnection = connect(&(qApp->getOverlays()), &Overlays::hoverLeaveOverlay, this, [=](OverlayID overlayID, const PointerEvent& event) {
            auto self = weakSelf.lock();
            if (!self) {
                return;
            }
            if (self->_pressed && overlayID == selfOverlayID) {
                PointerEvent endEvent(PointerEvent::Release, event.getID(), event.getPos2D(), event.getPos3D(), event.getNormal(), event.getDirection(),
                                      event.getButton(), event.getButtons(), event.getKeyboardModifiers());
                forwardPointerEvent(overlayID, event);
            }
        }, Qt::DirectConnection);

        _emitScriptEventConnection = connect(this, &Web3DOverlay::scriptEventReceived, _webSurface.data(), &OffscreenQmlSurface::emitScriptEvent);
        _webEventReceivedConnection = connect(_webSurface.data(), &OffscreenQmlSurface::webEventReceived, this, &Web3DOverlay::webEventReceived);
    } else {
        if (_currentMaxFPS != _desiredMaxFPS) {
            setMaxFPS(_desiredMaxFPS);
        }
    }

    if (_mayNeedResize) {
        _mayNeedResize = false;
        _webSurface->resize(QSize(_resolution.x, _resolution.y));
    }

    if (!_visible || !getParentVisible()) {
        return;
    }

    vec2 halfSize = getSize() / 2.0f;
    vec4 color(toGlm(getColor()), getAlpha());

    Transform transform = getTransform();

    // FIXME: applyTransformTo causes tablet overlay to detach from tablet entity.
    // Perhaps rather than deleting the following code it should be run only if isFacingAvatar() is true?
    /*
    applyTransformTo(transform, true);
    setTransform(transform);
    */

    if (glm::length2(getDimensions()) != 1.0f) {
        transform.postScale(vec3(getDimensions(), 1.0f));
    }

    if (!_texture) {
        auto webSurface = _webSurface;
        _texture = gpu::Texture::createExternal(OffscreenQmlSurface::getDiscardLambda());
        _texture->setSource(__FUNCTION__);
    }
    OffscreenQmlSurface::TextureAndFence newTextureAndFence;
    bool newTextureAvailable = _webSurface->fetchTexture(newTextureAndFence);
    if (newTextureAvailable) {
        _texture->setExternalTexture(newTextureAndFence.first, newTextureAndFence.second);
    }

    Q_ASSERT(args->_batch);
    gpu::Batch& batch = *args->_batch;
    batch.setResourceTexture(0, _texture);
    batch.setModelTransform(transform);
    auto geometryCache = DependencyManager::get<GeometryCache>();
    if (color.a < OPAQUE_ALPHA_THRESHOLD) {
        geometryCache->bindTransparentWebBrowserProgram(batch, _isAA);
    } else {
        geometryCache->bindOpaqueWebBrowserProgram(batch, _isAA);
    }
    geometryCache->renderQuad(batch, halfSize * -1.0f, halfSize, vec2(0), vec2(1), color, _geometryId);
    batch.setResourceTexture(0, args->_whiteTexture); // restore default white color after me
}

const render::ShapeKey Web3DOverlay::getShapeKey() {
    auto builder = render::ShapeKey::Builder().withoutCullFace().withDepthBias();
    if (getAlpha() != 1.0f) {
        builder.withTranslucent();
    }
    return builder.build();
}

QObject* Web3DOverlay::getEventHandler() {
    if (!_webSurface) {
        return nullptr;
    }
    return _webSurface->getEventHandler();
}

void Web3DOverlay::setProxyWindow(QWindow* proxyWindow) {
    if (!_webSurface) {
        return;
    }

    _webSurface->setProxyWindow(proxyWindow);
}

void Web3DOverlay::handlePointerEvent(const PointerEvent& event) {
    if (_inputMode == Touch) {
        handlePointerEventAsTouch(event);
    } else {
        handlePointerEventAsMouse(event);
    }
}

void Web3DOverlay::handlePointerEventAsTouch(const PointerEvent& event) {
    if (!_webSurface) {
        return;
    }

    glm::vec2 windowPos = event.getPos2D() * (METERS_TO_INCHES * _dpi);
    QPointF windowPoint(windowPos.x, windowPos.y);

    if (event.getType() == PointerEvent::Press && event.getButton() == PointerEvent::PrimaryButton) {
        this->_pressed = true;
    } else if (event.getType() == PointerEvent::Release && event.getButton() == PointerEvent::PrimaryButton) {
        this->_pressed = false;
    }

    QEvent::Type touchType;
    Qt::TouchPointState touchPointState;
    QEvent::Type mouseType;

    Qt::MouseButton button = Qt::NoButton;
    Qt::MouseButtons buttons = Qt::NoButton;
    if (event.getButton() == PointerEvent::PrimaryButton) {
        button = Qt::LeftButton;
    }
    if (event.getButtons() & PointerEvent::PrimaryButton) {
        buttons |= Qt::LeftButton;
    }

    switch (event.getType()) {
        case PointerEvent::Press:
            touchType = QEvent::TouchBegin;
            touchPointState = Qt::TouchPointPressed;
            mouseType = QEvent::MouseButtonPress;
            break;
        case PointerEvent::Release:
            touchType = QEvent::TouchEnd;
            touchPointState = Qt::TouchPointReleased;
            mouseType = QEvent::MouseButtonRelease;
            break;
        case PointerEvent::Move:
            touchType = QEvent::TouchUpdate;
            touchPointState = Qt::TouchPointMoved;
            mouseType = QEvent::MouseMove;

            if (((event.getButtons() & PointerEvent::PrimaryButton) > 0) != this->_pressed) {
                // Mouse was pressed/released while off the overlay; convert touch and mouse events to press/release to reflect
                // current mouse/touch status.
                this->_pressed = !this->_pressed;
                if (this->_pressed) {
                    touchType = QEvent::TouchBegin;
                    touchPointState = Qt::TouchPointPressed;
                    mouseType = QEvent::MouseButtonPress;

                } else {
                    touchType = QEvent::TouchEnd;
                    touchPointState = Qt::TouchPointReleased;
                    mouseType = QEvent::MouseButtonRelease;

                }
                button = Qt::LeftButton;
                buttons |= Qt::LeftButton;
            }

            break;
        default:
            return;
    }

    //do not send secondary button events to tablet
    if (event.getButton() == PointerEvent::SecondaryButton ||
            //do not block composed events
            event.getButtons() == PointerEvent::SecondaryButton) {
        return;
    }

    QTouchEvent::TouchPoint point;
    point.setId(event.getID());
    point.setState(touchPointState);
    point.setPos(windowPoint);
    point.setScreenPos(windowPoint);
    QList<QTouchEvent::TouchPoint> touchPoints;
    touchPoints.push_back(point);

    QTouchEvent* touchEvent = new QTouchEvent(touchType, &_touchDevice, event.getKeyboardModifiers());
    touchEvent->setWindow(_webSurface->getWindow());
    touchEvent->setTarget(_webSurface->getRootItem());
    touchEvent->setTouchPoints(touchPoints);
    touchEvent->setTouchPointStates(touchPointState);

    QCoreApplication::postEvent(_webSurface->getWindow(), touchEvent);

    if (this->_pressed && event.getType() == PointerEvent::Move) {	
        return;
    }
    // Send mouse events to the Web surface so that HTML dialog elements work with mouse press and hover.
    // FIXME: Scroll bar dragging is a bit unstable in the tablet (content can jump up and down at times). 
    // This may be improved in Qt 5.8. Release notes: "Cleaned up touch and mouse event delivery".

    QMouseEvent* mouseEvent = new QMouseEvent(mouseType, windowPoint, windowPoint, windowPoint, button, buttons, Qt::NoModifier);
    QCoreApplication::postEvent(_webSurface->getWindow(), mouseEvent);
}

void Web3DOverlay::handlePointerEventAsMouse(const PointerEvent& event) {
    if (!_webSurface) {
        return;
    }

    glm::vec2 windowPos = event.getPos2D() * (METERS_TO_INCHES * _dpi);
    QPointF windowPoint(windowPos.x, windowPos.y);

    if (event.getType() == PointerEvent::Press) {
        this->_pressed = true;
    } else if (event.getType() == PointerEvent::Release) {
        this->_pressed = false;
    }

    Qt::MouseButtons buttons = Qt::NoButton;
    if (event.getButtons() & PointerEvent::PrimaryButton) {
        buttons |= Qt::LeftButton;
    }

    Qt::MouseButton button = Qt::NoButton;
    if (event.getButton() == PointerEvent::PrimaryButton) {
        button = Qt::LeftButton;
    }

    QEvent::Type type;
    switch (event.getType()) {
        case PointerEvent::Press:
            type = QEvent::MouseButtonPress;
            break;
        case PointerEvent::Release:
            type = QEvent::MouseButtonRelease;
            break;
        case PointerEvent::Move:
            type = QEvent::MouseMove;
            break;
        default:
            return;
    }

    QMouseEvent* mouseEvent = new QMouseEvent(type, windowPoint, windowPoint, windowPoint, button, buttons, Qt::NoModifier);
    QCoreApplication::postEvent(_webSurface->getWindow(), mouseEvent);
}

void Web3DOverlay::setProperties(const QVariantMap& properties) {
    Billboard3DOverlay::setProperties(properties);

    auto urlValue = properties["url"];
    if (urlValue.isValid()) {
        QString newURL = urlValue.toString();
        if (newURL != _url) {
            setURL(newURL);
        }
    }

    auto scriptURLValue = properties["scriptURL"];
    if (scriptURLValue.isValid()) {
        QString newScriptURL = scriptURLValue.toString();
        if (newScriptURL != _scriptURL) {
            setScriptURL(newScriptURL);
        }
    }

    auto resolution = properties["resolution"];
    if (resolution.isValid()) {
        bool valid;
        auto res = vec2FromVariant(resolution, valid);
        if (valid) {
            _resolution = res;
        }
    }

    auto dpi = properties["dpi"];
    if (dpi.isValid()) {
        _dpi = dpi.toFloat();
    }

    auto maxFPS = properties["maxFPS"];
    if (maxFPS.isValid()) {
        _desiredMaxFPS = maxFPS.toInt();
    }

    auto showKeyboardFocusHighlight = properties["showKeyboardFocusHighlight"];
    if (showKeyboardFocusHighlight.isValid()) {
        _showKeyboardFocusHighlight = showKeyboardFocusHighlight.toBool();
    }

    auto inputModeValue = properties["inputMode"];
    if (inputModeValue.isValid()) {
        QString inputModeStr = inputModeValue.toString();
        if (inputModeStr == "Mouse") {
            _inputMode = Mouse;
        } else {
            _inputMode = Touch;
        }
    }

    _mayNeedResize = true;
}

QVariant Web3DOverlay::getProperty(const QString& property) {
    if (property == "url") {
        return _url;
    }
    if (property == "scriptURL") {
        return _scriptURL;
    }
    if (property == "resolution") {
        return vec2toVariant(_resolution);
    }
    if (property == "dpi") {
        return _dpi;
    }
    if (property == "maxFPS") {
        return _desiredMaxFPS;
    }
    if (property == "showKeyboardFocusHighlight") {
        return _showKeyboardFocusHighlight;
    }

    if (property == "inputMode") {
        if (_inputMode == Mouse) {
            return QVariant("Mouse");
        } else {
            return QVariant("Touch");
        }
    }
    return Billboard3DOverlay::getProperty(property);
}

void Web3DOverlay::setURL(const QString& url) {
    _url = url;
    if (_webSurface) {
        AbstractViewStateInterface::instance()->postLambdaEvent([this, url] {
            loadSourceURL();
        });
    }
}

void Web3DOverlay::setScriptURL(const QString& scriptURL) {
    _scriptURL = scriptURL;
    if (_webSurface) {
        AbstractViewStateInterface::instance()->postLambdaEvent([this, scriptURL] {
            _webSurface->getRootItem()->setProperty("scriptURL", scriptURL);
        });
    }
}

glm::vec2 Web3DOverlay::getSize() {
    return _resolution / _dpi * INCHES_TO_METERS * getDimensions();
};

bool Web3DOverlay::findRayIntersection(const glm::vec3& origin, const glm::vec3& direction, float& distance, BoxFace& face, glm::vec3& surfaceNormal) {
    // FIXME - face and surfaceNormal not being returned

    // Don't call applyTransformTo() or setTransform() here because this code runs too frequently.

    // Produce the dimensions of the overlay based on the image's aspect ratio and the overlay's scale.
    return findRayRectangleIntersection(origin, direction, getRotation(), getPosition(), getSize(), distance);
}

Web3DOverlay* Web3DOverlay::createClone() const {
    return new Web3DOverlay(this);
}

void Web3DOverlay::emitScriptEvent(const QVariant& message) {
    if (QThread::currentThread() != thread()) {
        QMetaObject::invokeMethod(this, "emitScriptEvent", Qt::QueuedConnection, Q_ARG(QVariant, message));
    } else {
        emit scriptEventReceived(message);
    }
}
