/****************************************************************************
**
** Copyright (C) 2022 The Qt Company Ltd.
** Contact: https://www.qt.io/licensing/
**
** This file is part of the Spatial Audio module of the Qt Toolkit.
**
** $QT_BEGIN_LICENSE:LGPL-NOGPL2$
** Commercial License Usage
** Licensees holding valid commercial Qt licenses may use this file in
** accordance with the commercial license agreement provided with the
** Software or, alternatively, in accordance with the terms contained in
** a written agreement between you and The Qt Company. For licensing terms
** and conditions see https://www.qt.io/terms-conditions. For further
** information use the contact form at https://www.qt.io/contact-us.
**
** GNU Lesser General Public License Usage
** Alternatively, this file may be used under the terms of the GNU Lesser
** General Public License version 3 as published by the Free Software
** Foundation and appearing in the file LICENSE.LGPL3 included in the
** packaging of this file. Please review the following information to
** ensure the GNU Lesser General Public License version 3 requirements
** will be met: https://www.gnu.org/licenses/lgpl-3.0.html.
**
** GNU General Public License Usage
** Alternatively, this file may be used under the terms of the GNU
** General Public License version 3 or (at your option) any later version
** approved by the KDE Free Qt Foundation. The licenses are as published by
** the Free Software Foundation and appearing in the file LICENSE.GPL3
** included in the packaging of this file. Please review the following
** information to ensure the GNU General Public License requirements will
** be met: https://www.gnu.org/licenses/gpl-3.0.html.
**
** $QT_END_LICENSE$
**
****************************************************************************/

#ifndef QSPATIALAUDIOENGINE_P_H
#define QSPATIALAUDIOENGINE_P_H

//
//  W A R N I N G
//  -------------
//
// This file is not part of the Qt API.  It exists for the convenience
// of other Qt classes.  This header file may change from version to
// version without notice, or even be removed.
//
// We mean it.
//

#include <qspatialaudioengine.h>
#include <qaudiodevice.h>
#include <qthread.h>
#include <qmutex.h>

namespace vraudio {
class ResonanceAudioApi;
}

QT_BEGIN_NAMESPACE

class QSpatialAudioSoundSource;
class QSpatialAudioStereoSource;
class QAudioSink;
class QAudioOutputStream;
class QAmbisonicDecoder;

class QSpatialAudioEnginePrivate
{
public:
    static QSpatialAudioEnginePrivate *get(QSpatialAudioEngine *engine) { return engine ? engine->d : nullptr; }

    static constexpr int bufferSize = 128;

    QSpatialAudioEnginePrivate();
    ~QSpatialAudioEnginePrivate();
    vraudio::ResonanceAudioApi *api = nullptr;
    int sampleRate = 44100;
    float masterVolume = 1.;
    QSpatialAudioEngine::OutputMode outputMode = QSpatialAudioEngine::Normal;
    bool roomEffectsEnabled = true;

    QMutex mutex;
    QAudioFormat format;
    QAudioDevice device;

    QThread audioThread;
    std::unique_ptr<QAudioOutputStream> outputStream;
    std::unique_ptr<QAmbisonicDecoder> ambisonicDecoder;

    QList<QSpatialAudioSoundSource *> sources;
    QList<QSpatialAudioStereoSource *> stereoSources;

    void addSpatialSound(QSpatialAudioSoundSource *sound);
    void removeSpatialSound(QSpatialAudioSoundSource *sound);
    void addStereoSound(QSpatialAudioStereoSource *sound);
    void removeStereoSound(QSpatialAudioStereoSource *sound);
};

QT_END_NAMESPACE

#endif
