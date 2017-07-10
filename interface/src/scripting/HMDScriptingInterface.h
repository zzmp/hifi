
//  HMDScriptingInterface.h
//  interface/src/scripting
//
//  Created by Thijs Wenker on 1/12/15.
//  Copyright 2015 High Fidelity, Inc.
//
//  Distributed under the Apache License, Version 2.0.
//  See the accompanying file LICENSE or http://www.apache.org/licenses/LICENSE-2.0.html
//

#ifndef hifi_HMDScriptingInterface_h
#define hifi_HMDScriptingInterface_h

#include <atomic>

#include <QtScript/QScriptValue>
class QScriptContext;
class QScriptEngine;

#include <GLMHelpers.h>
#include <DependencyManager.h>
#include <display-plugins/AbstractHMDScriptingInterface.h>


class HMDScriptingInterface : public AbstractHMDScriptingInterface, public Dependency {
    Q_OBJECT
    Q_PROPERTY(glm::vec3 position READ getPosition)
    Q_PROPERTY(glm::quat orientation READ getOrientation)
    Q_PROPERTY(bool mounted READ isMounted NOTIFY mountedChanged)
    Q_PROPERTY(bool showTablet READ getShouldShowTablet)
    Q_PROPERTY(QUuid tabletID READ getCurrentTabletFrameID WRITE setCurrentTabletFrameID)
    Q_PROPERTY(QUuid homeButtonID READ getCurrentHomeButtonID WRITE setCurrentHomeButtonID)
    Q_PROPERTY(QUuid tabletScreenID READ getCurrentTabletScreenID WRITE setCurrentTabletScreenID)

public:
    Q_INVOKABLE glm::vec3 calculateRayUICollisionPoint(const glm::vec3& position, const glm::vec3& direction) const;
    Q_INVOKABLE glm::vec2 overlayFromWorldPoint(const glm::vec3& position) const;
    Q_INVOKABLE glm::vec3 worldPointFromOverlay(const glm::vec2& overlay) const;
    Q_INVOKABLE glm::vec2 sphericalToOverlay(const glm::vec2 & sphericalPos) const;
    Q_INVOKABLE glm::vec2 overlayToSpherical(const glm::vec2 & overlayPos) const;
    Q_INVOKABLE QString preferredAudioInput() const;
    Q_INVOKABLE QString preferredAudioOutput() const;

    Q_INVOKABLE bool isHMDAvailable(const QString& name = "");
    Q_INVOKABLE bool isHeadControllerAvailable(const QString& name = "");
    Q_INVOKABLE bool isHandControllerAvailable(const QString& name = "");
    Q_INVOKABLE bool isSubdeviceContainingNameAvailable(const QString& name);

    Q_INVOKABLE void requestShowHandControllers();
    Q_INVOKABLE void requestHideHandControllers();
    Q_INVOKABLE bool shouldShowHandControllers() const;

    Q_INVOKABLE bool setHandLasers(int hands, bool enabled, const glm::vec4& color, const glm::vec3& direction);
    Q_INVOKABLE void disableHandLasers(int hands);

    Q_INVOKABLE bool setExtraLaser(const glm::vec3& worldStart, bool enabled, const glm::vec4& color, const glm::vec3& direction);
    Q_INVOKABLE void disableExtraLaser();


    /// Suppress the activation of any on-screen keyboard so that a script operation will
    /// not be interrupted by a keyboard popup
    /// Returns false if there is already an active keyboard displayed.
    /// Clients should re-enable the keyboard when the operation is complete and ensure
    /// that they balance any call to suppressKeyboard() that returns true with a corresponding
    /// call to unsuppressKeyboard() within a reasonable amount of time
    Q_INVOKABLE bool suppressKeyboard();

    /// Enable the keyboard following a suppressKeyboard call
    Q_INVOKABLE void unsuppressKeyboard();

    /// Query the display plugin to determine the current VR keyboard visibility
    Q_INVOKABLE bool isKeyboardVisible();

    // rotate the overlay UI sphere so that it is centered about the the current HMD position and orientation
    Q_INVOKABLE void centerUI();

    Q_INVOKABLE void closeTablet();

    Q_INVOKABLE void openTablet();

signals:
    bool shouldShowHandControllersChanged();
    void mountedChanged();

public:
    HMDScriptingInterface();
    static QScriptValue getHUDLookAtPosition2D(QScriptContext* context, QScriptEngine* engine);
    static QScriptValue getHUDLookAtPosition3D(QScriptContext* context, QScriptEngine* engine);

    bool isMounted() const;

    void toggleShouldShowTablet() { _showTablet = !_showTablet; }
    void setShouldShowTablet(bool value) { _showTablet = value; }
    bool getShouldShowTablet() const { return _showTablet; }

    void setCurrentTabletFrameID(QUuid tabletID) { _tabletUIID = tabletID; }
    QUuid getCurrentTabletFrameID() const { return _tabletUIID; }

    void setCurrentHomeButtonID(QUuid homeButtonID) { _homeButtonID = homeButtonID; }
    QUuid getCurrentHomeButtonID() const { return _homeButtonID; }

    void setCurrentTabletScreenID(QUuid tabletID) { _tabletScreenID = tabletID; }
    QUuid getCurrentTabletScreenID() const { return _tabletScreenID; }

private:
    bool _showTablet { false };
    QUuid _tabletUIID; // this is the entityID of the tablet frame
    QUuid _tabletScreenID; // this is the overlayID which is part of (a child of) the tablet-ui.
    QUuid _homeButtonID;
    QUuid _tabletEntityID;

    // Get the position of the HMD
    glm::vec3 getPosition() const;

    // Get the orientation of the HMD
    glm::quat getOrientation() const;

    bool getHUDLookAtPosition3D(glm::vec3& result) const;
    glm::mat4 getWorldHMDMatrix() const;
    std::atomic<int> _showHandControllersCount { 0 };
};

#endif // hifi_HMDScriptingInterface_h
