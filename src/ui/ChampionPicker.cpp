#include "ui/ChampionPicker.h"

#include "services/ChampionRepository.h"

#include <QApplication>
#include <QComboBox>
#include <QGridLayout>
#include <QEasingCurve>
#include <QFrame>
#include <QGraphicsDropShadowEffect>
#include <QHBoxLayout>
#include <QLabel>
#include <QLineEdit>
#include <QParallelAnimationGroup>
#include <QPaintEvent>
#include <QPainter>
#include <QPainterPath>
#include <QPropertyAnimation>
#include <QPushButton>
#include <QPixmap>
#include <QScrollArea>
#include <QSignalBlocker>
#include <QRegion>
#include <QResizeEvent>
#include <QShowEvent>
#include <QTimer>
#include <QVBoxLayout>
#include <algorithm>

namespace Janna {
namespace {

void addPickerShadow(QWidget *widget)
{
    auto *shadow = new QGraphicsDropShadowEffect(widget);
    shadow->setBlurRadius(18);
    shadow->setOffset(0, 4);
    shadow->setColor(QColor(0, 0, 0, 34));
    widget->setGraphicsEffect(shadow);
}

QWidget *championTile(const Champion &champion, const QString &alias,
                      const QStringList &types, const QPixmap &image)
{
    auto *tile = new QWidget;
    auto *layout = new QVBoxLayout(tile);
    layout->setContentsMargins(8, 7, 8, 7);
    layout->setSpacing(2);
    auto *portrait = new QLabel;
    portrait->setObjectName("portrait");
    portrait->setFixedSize(42, 42);
    portrait->setAlignment(Qt::AlignCenter);
    if (image.isNull()) {
        portrait->setText("?");
    } else {
        portrait->setPixmap(image.scaled(portrait->size(), Qt::KeepAspectRatioByExpanding, Qt::SmoothTransformation));
    }
    layout->addWidget(portrait, 0, Qt::AlignCenter);
    auto *name = new QLabel(champion.name);
    name->setAlignment(Qt::AlignCenter);
    layout->addWidget(name);
    if (!alias.isEmpty()) {
        auto *aliasLabel = new QLabel(alias);
        aliasLabel->setObjectName("alias");
        aliasLabel->setAlignment(Qt::AlignCenter);
        layout->addWidget(aliasLabel);
    }
    if (!types.isEmpty()) {
        auto *typeLabel = new QLabel(types.join(QStringLiteral(" / ")));
        typeLabel->setObjectName("championType");
        typeLabel->setAlignment(Qt::AlignCenter);
        typeLabel->setWordWrap(true);
        layout->addWidget(typeLabel);
    }
    return tile;
}
}

ChampionPicker::ChampionPicker(ChampionRepository &repository, QWidget *parent)
    : QDialog(parent), repository_(repository)
{
    setWindowTitle("选择英雄");
    setObjectName("championPicker");
    setWindowFlags(Qt::Dialog | Qt::FramelessWindowHint);
    // The dialog itself is opaque; the mask below supplies the rounded
    // silhouette without allowing the page behind it to bleed through.
    setAttribute(Qt::WA_TranslucentBackground, false);
    setAttribute(Qt::WA_OpaquePaintEvent);
    setAttribute(Qt::WA_StyledBackground);
    setModal(true);
    resize(900, 620);
    auto *root = new QVBoxLayout(this);
    root->setContentsMargins(28, 24, 28, 20);
    root->setSpacing(18);
    auto *heading = new QHBoxLayout;
    auto *headingText = new QVBoxLayout;
    auto *eyebrow = new QLabel("CHAMPION POOL"); eyebrow->setObjectName("pickerEyebrow");
    auto *headingTitle = new QLabel("选择英雄"); headingTitle->setObjectName("pickerHeading");
    headingText->addWidget(eyebrow); headingText->addWidget(headingTitle); heading->addLayout(headingText); heading->addStretch();
    auto *close = new QPushButton("×"); close->setObjectName("pickerClose"); close->setFixedSize(34, 34); heading->addWidget(close);
    root->addLayout(heading);
    auto *body = new QHBoxLayout;
    body->setSpacing(18);

    auto *catalogPanel = new QFrame; catalogPanel->setObjectName("pickerPanel"); addPickerShadow(catalogPanel); auto *left = new QVBoxLayout(catalogPanel); left->setContentsMargins(16, 16, 16, 16);
    auto *catalogTitle = new QLabel("英雄池"); catalogTitle->setObjectName("pickerSection"); left->addWidget(catalogTitle);
    filter_ = new QLineEdit;
    filter_->setPlaceholderText("搜索英雄名、英雄称号或自定义外号");
    filter_->setClearButtonEnabled(true);
    left->addWidget(filter_);
    auto *typeRow = new QHBoxLayout;
    typeFilterRow_ = typeRow;
    auto *typeLabel = new QLabel(QStringLiteral("英雄定位"));
    typeLabel->setObjectName("pickerFilterLabel");
    typeRow->addWidget(typeLabel);
    typeFilter_ = new QComboBox;
    typeFilter_->setObjectName("championTypeFilter");
    typeFilter_->setToolTip(QStringLiteral("按英雄定位筛选；与上路、打野等分路独立"));
    typeRow->addWidget(typeFilter_, 1);
    left->addLayout(typeRow);
    auto *laneRow = new QHBoxLayout;
    laneFilterRow_ = laneRow;
    auto *laneLabel = new QLabel(QStringLiteral("分路"));
    laneLabel->setObjectName("pickerFilterLabel");
    laneRow->addWidget(laneLabel);
    laneFilter_ = new QComboBox;
    laneFilter_->setObjectName("championLaneFilter");
    laneFilter_->setToolTip(QStringLiteral("按 OP.GG 分路筛选；可与英雄定位同时使用"));
    laneRow->addWidget(laneFilter_, 1);
    laneRow->setEnabled(false);
    laneRow->setContentsMargins(0, 0, 0, 0);
    // Keep the layout alive as a child of the catalog panel and hide it until
    // the OP.GG window explicitly opts into lane filtering.
    left->addLayout(laneRow);
    laneRow->setObjectName(QStringLiteral("championLaneFilterRow"));
    for (int index = 0; index < laneRow->count(); ++index) {
        if (QWidget *widget = laneRow->itemAt(index)->widget()) widget->setVisible(false);
    }
    auto *scroll = new QScrollArea;
    scroll->setObjectName("championCatalogScroll");
    scroll->setAttribute(Qt::WA_StyledBackground);
    scroll->setAutoFillBackground(true);
    scroll->viewport()->setObjectName("championCatalogViewport");
    scroll->viewport()->setAttribute(Qt::WA_StyledBackground);
    scroll->viewport()->setAutoFillBackground(true);
    scroll->setWidgetResizable(true);
    scroll->setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
    auto *catalog = new QWidget;
    catalog->setObjectName("championCatalog");
    catalog->setAttribute(Qt::WA_StyledBackground);
    catalog->setAutoFillBackground(true);
    grid_ = new QGridLayout(catalog);
    grid_->setAlignment(Qt::AlignTop | Qt::AlignLeft);
    grid_->setSpacing(10);
    scroll->setWidget(catalog);
    left->addWidget(scroll);
    body->addWidget(catalogPanel, 3);

    auto *selectionPanel = new QFrame; selectionPanel->setObjectName("pickerPanel"); addPickerShadow(selectionPanel); auto *right = new QVBoxLayout(selectionPanel); right->setContentsMargins(16, 16, 16, 16);
    auto *title = new QLabel("已选取的英雄");
    title->setObjectName("sectionTitle");
    right->addWidget(title);
    auto *selectedView = new QWidget;
    selectedView->setObjectName("selectedChampionView");
    selectedView->setAttribute(Qt::WA_StyledBackground);
    selectedView->setAutoFillBackground(true);
    selectedLayout_ = new QVBoxLayout(selectedView);
    selectedLayout_->setAlignment(Qt::AlignTop);
    right->addWidget(selectedView, 1);
    auto *aliasTitle = new QLabel("自定义外号");
    aliasTitle->setObjectName("sectionTitle");
    right->addWidget(aliasTitle);
    alias_ = new QLineEdit;
    alias_->setPlaceholderText("选中英雄后设置外号");
    alias_->setEnabled(false);
    right->addWidget(alias_);
    auto *saveAlias = new QPushButton("保存外号");
    saveAlias->setObjectName("secondary");
    saveAlias->setEnabled(false);
    right->addWidget(saveAlias, 0, Qt::AlignLeft);
    body->addWidget(selectionPanel, 2);
    root->addLayout(body, 1);

    auto *actions = new QHBoxLayout;
    actions->addStretch();
    auto *cancel = new QPushButton("取消");
    auto *confirm = new QPushButton("确认");
    cancel->setObjectName("secondary"); confirm->setObjectName("primary");
    actions->addWidget(cancel); actions->addWidget(confirm);
    root->addLayout(actions);
    connect(filter_, &QLineEdit::textChanged, this, &ChampionPicker::rebuild);
    connect(typeFilter_, qOverload<int>(&QComboBox::currentIndexChanged), this, &ChampionPicker::rebuild);
    connect(laneFilter_, qOverload<int>(&QComboBox::currentIndexChanged), this, &ChampionPicker::rebuild);
    connect(&repository_, &ChampionRepository::championsChanged, this, &ChampionPicker::rebuild);
    connect(&repository_, &ChampionRepository::championsChanged, this, &ChampionPicker::populateTypeOptions);
    assetRefreshTimer_ = new QTimer(this);
    assetRefreshTimer_->setSingleShot(true);
    assetRefreshTimer_->setInterval(120);
    connect(assetRefreshTimer_, &QTimer::timeout, this, &ChampionPicker::rebuild);
    connect(&repository_, &ChampionRepository::championPortraitsChanged, this, [this] {
        if (!assetRefreshTimer_->isActive()) assetRefreshTimer_->start();
    });
    connect(cancel, &QPushButton::clicked, this, &QDialog::reject);
    connect(confirm, &QPushButton::clicked, this, &QDialog::accept);
    connect(close, &QPushButton::clicked, this, &QDialog::reject);
    connect(alias_, &QLineEdit::textChanged, saveAlias, [saveAlias](const QString &) { saveAlias->setEnabled(true); });
    connect(saveAlias, &QPushButton::clicked, this, &ChampionPicker::saveAlias);
    populateTypeOptions();
    rebuild();
}

void ChampionPicker::setAllowedChampionIds(const QSet<int> &ids)
{
    allowedChampionIds_ = ids;
    rebuild();
}

void ChampionPicker::setPoolRestricted(const bool restricted)
{
    poolRestricted_ = restricted;
    rebuild();
}

void ChampionPicker::setSelectedChampionIds(const QList<int> &ids)
{
    selected_.clear();
    while (auto *item = selectedLayout_->takeAt(0)) {
        delete item->widget();
        delete item;
    }
    for (const int id : ids) {
        if (id > 0 && (!poolRestricted_ || allowedChampionIds_.contains(id))) selectChampion(id);
    }
    // Programmatic selection is also used when restoring the current choice
    // from the OP.GG window. Rebuild synchronously so tile properties reflect
    // the restored selection before the caller inspects or displays the picker.
    rebuild();
}

void ChampionPicker::setLaneFilter(const QString &lane, const QSet<int> &laneChampionIds)
{
    laneFilterEnabled_ = true;
    laneFilterValue_ = lane.trimmed().toLower();
    if (laneFilterValue_.isEmpty()) laneFilterValue_ = QStringLiteral("default");
    laneChampionIds_ = laneChampionIds;
    if (typeFilterRow_) {
        for (int index = 0; index < typeFilterRow_->count(); ++index) {
            if (QWidget *widget = typeFilterRow_->itemAt(index)->widget()) widget->setVisible(true);
        }
    }
    if (laneFilterRow_) {
        laneFilterRow_->setEnabled(true);
        for (int index = 0; index < laneFilterRow_->count(); ++index) {
            if (QWidget *widget = laneFilterRow_->itemAt(index)->widget()) widget->setVisible(true);
        }
    }
    populateLaneOptions();
    rebuild();
}

void ChampionPicker::paintEvent(QPaintEvent *event)
{
    Q_UNUSED(event);
    QPainter painter(this);
    painter.setRenderHint(QPainter::Antialiasing);
    const QColor surface = qApp->property("jannaSurface").value<QColor>();
    const QColor border = qApp->property("jannaBorder").value<QColor>();
    // Keep the dialog surface explicitly painted.  In the default light theme
    // the resolved surface is white, while the mask clips all four corners.
    painter.setBrush(surface.isValid() ? surface : QColor(Qt::white));
    painter.setPen(QPen(border.isValid() ? border : QColor("#DDE5DF"), 1));
    painter.drawRoundedRect(rect().adjusted(0, 0, -1, -1), 12, 12);
}

void ChampionPicker::resizeEvent(QResizeEvent *event)
{
    QDialog::resizeEvent(event);
    QPainterPath path;
    path.addRoundedRect(rect().adjusted(0, 0, -1, -1), 12, 12);
    setMask(QRegion(path.toFillPolygon().toPolygon()));
}

void ChampionPicker::showEvent(QShowEvent *event)
{
    QDialog::showEvent(event);
    QPainterPath path;
    path.addRoundedRect(rect().adjusted(0, 0, -1, -1), 12, 12);
    setMask(QRegion(path.toFillPolygon().toPolygon()));
    if (opening_) return;
    opening_ = true;
    const QPoint destination = pos();
    setWindowOpacity(0.0);
    move(destination + QPoint(0, 12));
    QTimer::singleShot(0, this, [this, destination] {
        if (!isVisible()) return;
        auto *animation = new QParallelAnimationGroup(this);
        auto *opacity = new QPropertyAnimation(this, "windowOpacity", animation);
        opacity->setStartValue(0.0);
        opacity->setEndValue(1.0);
        auto *position = new QPropertyAnimation(this, "pos", animation);
        position->setStartValue(pos());
        position->setEndValue(destination);
        for (QPropertyAnimation *part : {opacity, position}) {
            part->setDuration(180);
            part->setEasingCurve(QEasingCurve::OutCubic);
            animation->addAnimation(part);
        }
        connect(animation, &QParallelAnimationGroup::finished, this, [this] { opening_ = false; });
        animation->start(QAbstractAnimation::DeleteWhenStopped);
    });
}

void ChampionPicker::done(const int result)
{
    if (closing_) return;
    closing_ = true;
    auto *animation = new QParallelAnimationGroup(this);
    auto *opacity = new QPropertyAnimation(this, "windowOpacity", animation);
    opacity->setStartValue(windowOpacity());
    opacity->setEndValue(0.0);
    auto *position = new QPropertyAnimation(this, "pos", animation);
    position->setStartValue(pos());
    position->setEndValue(pos() + QPoint(0, 8));
    for (QPropertyAnimation *part : {opacity, position}) {
        part->setDuration(140);
        part->setEasingCurve(QEasingCurve::InCubic);
        animation->addAnimation(part);
    }
    connect(animation, &QParallelAnimationGroup::finished, this, [this, result] {
        QDialog::done(result);
        setWindowOpacity(1.0);
        closing_ = false;
    });
    animation->start(QAbstractAnimation::DeleteWhenStopped);
}

void ChampionPicker::rebuild()
{
    while (auto *item = grid_->takeAt(0)) { delete item->widget(); delete item; }
    const QString query = filter_->text().trimmed();
    // Position (lane) and champion class (role) are independent dimensions;
    // keep the role constraint active even when OP.GG supplies lane data.
    const QString selectedType = typeFilter_
        ? typeFilter_->currentData().toString() : QString{};
    const QString selectedLane = laneFilterEnabled_ && laneFilter_
        ? laneFilter_->currentData().toString() : QString{};
    int position = 0;
    for (const Champion &champion : repository_.champions()) {
        if (poolRestricted_ && !allowedChampionIds_.contains(champion.id)) continue;
        if (!selectedType.isEmpty()
            && !repository_.championTypeLabelsFor(champion.id).contains(selectedType)) continue;
        if (!selectedLane.isEmpty() && selectedLane != QStringLiteral("default")
            && !laneChampionIds_.isEmpty() && !laneChampionIds_.contains(champion.id)
            && !selected_.contains(champion.id)) continue;
        const QString alias = repository_.aliasFor(champion.id);
        if (!query.isEmpty() && !champion.name.contains(query, Qt::CaseInsensitive)
            && !champion.title.contains(query, Qt::CaseInsensitive)
            && !champion.key.contains(query, Qt::CaseInsensitive)
            && !alias.contains(query, Qt::CaseInsensitive)) continue;
        auto *button = new QPushButton;
        button->setObjectName("championTile");
        const bool selected = selected_.contains(champion.id);
        button->setProperty("championSelected", selected);
        button->setToolTip(champion.name + " - " + champion.title
            + (selected ? QStringLiteral("（已选择）") : QString{}));
        // A champion may have two independent class tags, such as AD fighter
        // and AD assassin. Reserve enough height for the wrapped tag row.
        button->setFixedSize(84, 136);
        auto *layout = new QVBoxLayout(button);
        layout->setContentsMargins(3, 3, 3, 3);
        layout->addWidget(championTile(champion, alias,
                                       repository_.championTypeLabelsFor(champion.id),
                                       repository_.portraitFor(champion.id)));
        grid_->addWidget(button, position / 6, position % 6);
        ++position;
        connect(button, &QPushButton::clicked, this, [this, id = champion.id] { selectChampion(id); });
    }
}

void ChampionPicker::populateTypeOptions()
{
    if (!typeFilter_) return;
    const QString current = typeFilter_->currentData().toString();
    QSignalBlocker blocker(typeFilter_);
    typeFilter_->clear();
    typeFilter_->addItem(QStringLiteral("全部定位"), QString{});
    QStringList types;
    for (const Champion &champion : repository_.champions()) {
        for (const QString &type : repository_.championTypeLabelsFor(champion.id)) {
            if (!types.contains(type)) types.append(type);
        }
    }
    std::sort(types.begin(), types.end(), [](const QString &left, const QString &right) {
        return left.localeAwareCompare(right) < 0;
    });
    for (const QString &type : types) typeFilter_->addItem(type, type);
    const int index = typeFilter_->findData(current);
    typeFilter_->setCurrentIndex(index >= 0 ? index : 0);
    if (grid_) rebuild();
}

void ChampionPicker::populateLaneOptions()
{
    if (!laneFilter_) return;
    QSignalBlocker blocker(laneFilter_);
    laneFilter_->clear();
    laneFilter_->addItem(QStringLiteral("全部分路"), QStringLiteral("default"));
    const QList<QPair<QString, QString>> lanes = {
        {QStringLiteral("上路"), QStringLiteral("top")},
        {QStringLiteral("打野"), QStringLiteral("jungle")},
        {QStringLiteral("中路"), QStringLiteral("mid")},
        {QStringLiteral("下路"), QStringLiteral("bot")},
        {QStringLiteral("辅助"), QStringLiteral("support")}
    };
    for (const auto &entry : lanes) laneFilter_->addItem(entry.first, entry.second);
    const int index = laneFilter_->findData(laneFilterValue_);
    laneFilter_->setCurrentIndex(index >= 0 ? index : 0);
}

void ChampionPicker::selectChampion(int id)
{
    const auto it = std::find_if(repository_.champions().cbegin(), repository_.champions().cend(), [id](const Champion &champion) { return champion.id == id; });
    if (it == repository_.champions().cend()) return;
    focusedChampionId_ = id;
    alias_->setEnabled(true);
    alias_->setText(repository_.aliasFor(id));
    if (selected_.contains(id)) return;
    selected_.append(id);
    auto *row = new QWidget;
    auto *layout = new QHBoxLayout(row);
    layout->setContentsMargins(0, 2, 0, 2);
    auto *portrait = new QLabel;
    portrait->setObjectName("portrait");
    portrait->setFixedSize(28, 28);
    portrait->setAlignment(Qt::AlignCenter);
    const QPixmap image = repository_.portraitFor(id);
    if (image.isNull()) portrait->setText("?");
    else portrait->setPixmap(image.scaled(portrait->size(), Qt::KeepAspectRatioByExpanding, Qt::SmoothTransformation));
    layout->addWidget(portrait);
    layout->addWidget(new QLabel(it->name));
    layout->addWidget(new QLabel(it->title));
    const QStringList types = repository_.championTypeLabelsFor(id);
    if (!types.isEmpty()) {
        auto *typeLabel = new QLabel(types.join(QStringLiteral(" / ")));
        typeLabel->setObjectName("championType");
        typeLabel->setWordWrap(true);
        layout->addWidget(typeLabel);
    }
    layout->addStretch();
    auto *remove = new QPushButton("×");
    remove->setObjectName("remove");
    layout->addWidget(remove);
    selectedLayout_->addWidget(row);
    connect(remove, &QPushButton::clicked, this, [this, row, id] {
        selected_.removeAll(id);
        selectedLayout_->removeWidget(row);
        row->deleteLater();
        QTimer::singleShot(0, this, &ChampionPicker::rebuild);
    });
    // The tile that triggered this slot is still emitting clicked(). Defer
    // rebuilding until the signal returns so its sender is not destroyed
    // underneath Qt's event delivery.
    QTimer::singleShot(0, this, &ChampionPicker::rebuild);
}

void ChampionPicker::saveAlias()
{
    if (!focusedChampionId_) return;
    repository_.setAlias(focusedChampionId_, alias_->text());
}
}
