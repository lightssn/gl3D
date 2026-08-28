#pragma once

#include <QMetaType>
#include <QVector>
#include <QString>
#include <QVector3D>

struct DebugResourceInfo {
    QString type;
    int count = 0;
    qint64 cpuBytes = 0;
    qint64 bytes = 0;
    bool queryable = true;
};

struct DebugSnapshot {
    QVector<DebugResourceInfo> resources;
    QVector<float> frameTimesMs;
    int fps = 0;
    int drawCalls = 0;
    int triangles = 0;
    float averageFrameMs = 0.0f;
    float lowFrameMs = 0.0f;
    float p95FrameMs = 0.0f;
    float p99FrameMs = 0.0f;
    float onePercentLowFps = 0.0f;
    float fov = 45.0f;
    float distance = 0.0f;
    QVector3D eye;
    QVector3D target;
    bool ortho = false;
    bool antialiasing = true;
    bool modelLoaded = false;
    QString renderer;
    QString glVersion;
};

Q_DECLARE_METATYPE(DebugSnapshot)
