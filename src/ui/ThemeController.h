#pragma once

#include <QObject>

namespace Janna {

class ThemeController : public QObject {
    Q_OBJECT

public:
    enum class Mode { Light, Dark, System };

    explicit ThemeController(QObject *parent = nullptr);
    Mode mode() const { return mode_; }

public slots:
    void setMode(Mode mode);

signals:
    void modeChanged(Mode mode);

private:
    void apply();
    bool useDarkPalette() const;
    Mode mode_ = Mode::Light;
};

} // namespace Janna
