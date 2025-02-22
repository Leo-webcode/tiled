/*
 * propertytypeseditor.cpp
 * Copyright 2016-2021, Thorbjørn Lindeijer <bjorn@lindeijer.nl>>
 *
 * This file is part of Tiled.
 *
 * This program is free software; you can redistribute it and/or modify it
 * under the terms of the GNU General Public License as published by the Free
 * Software Foundation; either version 2 of the License, or (at your option)
 * any later version.
 *
 * This program is distributed in the hope that it will be useful, but WITHOUT
 * ANY WARRANTY; without even the implied warranty of MERCHANTABILITY or
 * FITNESS FOR A PARTICULAR PURPOSE.  See the GNU General Public License for
 * more details.
 *
 * You should have received a copy of the GNU General Public License along with
 * this program. If not, see <http://www.gnu.org/licenses/>.
 */

#include "propertytypeseditor.h"
#include "ui_propertytypeseditor.h"

#include "addpropertydialog.h"
#include "custompropertieshelper.h"
#include "object.h"
#include "preferences.h"
#include "project.h"
#include "projectmanager.h"
#include "propertytypesmodel.h"
#include "savefile.h"
#include "session.h"
#include "utils.h"
#include "varianteditorfactory.h"
#include "variantpropertymanager.h"

#include <QCheckBox>
#include <QCloseEvent>
#include <QComboBox>
#include <QFileDialog>
#include <QFormLayout>
#include <QInputDialog>
#include <QJsonDocument>
#include <QMessageBox>
#include <QScopedValueRollback>
#include <QStringListModel>
#include <QToolBar>

#include <QtTreePropertyBrowser>

namespace Tiled {

static QToolBar *createSmallToolBar(QWidget *parent)
{
    auto toolBar = new QToolBar(parent);
    toolBar->setToolButtonStyle(Qt::ToolButtonTextBesideIcon);
    toolBar->setIconSize(Utils::smallIconSize());
    return toolBar;
}

static bool confirm(const QString &title, const QString& text, QWidget *parent)
{
    return QMessageBox::warning(parent, title, text,
                                QMessageBox::Yes | QMessageBox::No,
                                QMessageBox::No) == QMessageBox::Yes;
}

PropertyTypesEditor::PropertyTypesEditor(QWidget *parent)
    : QDialog(parent)
    , mUi(new Ui::PropertyTypesEditor)
    , mPropertyTypesModel(new PropertyTypesModel(this))
    , mValuesModel(new QStringListModel(this))
{
    mUi->setupUi(this);

    resize(Utils::dpiScaled(size()));

    mUi->propertyTypesView->setModel(mPropertyTypesModel);

    mAddEnumPropertyTypeAction = new QAction(this);
    mAddClassPropertyTypeAction = new QAction(this);
    mRemovePropertyTypeAction = new QAction(this);
    mAddValueAction = new QAction(this);
    mRemoveValueAction = new QAction(this);
    mAddMemberAction = new QAction(this);
    mRemoveMemberAction = new QAction(this);
    mRenameMemberAction = new QAction(this);
    mExportAction = new QAction(this);
    mImportAction = new QAction(this);

    QIcon addIcon(QStringLiteral(":/images/22/add.png"));
    QIcon removeIcon(QStringLiteral(":/images/22/remove.png"));
    QIcon renameIcon(QStringLiteral(":/images/16/rename.png"));

    mAddEnumPropertyTypeAction->setIcon(addIcon);
    mAddClassPropertyTypeAction->setIcon(addIcon);
    mRemovePropertyTypeAction->setEnabled(false);
    mRemovePropertyTypeAction->setIcon(removeIcon);
    mRemovePropertyTypeAction->setPriority(QAction::LowPriority);

    mAddValueAction->setEnabled(false);
    mAddValueAction->setIcon(addIcon);
    mRemoveValueAction->setEnabled(false);
    mRemoveValueAction->setIcon(removeIcon);
    mRemoveValueAction->setPriority(QAction::LowPriority);

    mAddMemberAction->setEnabled(false);
    mAddMemberAction->setIcon(addIcon);
    mRemoveMemberAction->setEnabled(false);
    mRemoveMemberAction->setIcon(removeIcon);
    mRemoveMemberAction->setPriority(QAction::LowPriority);
    mRenameMemberAction->setEnabled(false);
    mRenameMemberAction->setIcon(renameIcon);
    mRenameMemberAction->setPriority(QAction::LowPriority);

    Utils::setThemeIcon(mAddEnumPropertyTypeAction, "add");
    Utils::setThemeIcon(mAddClassPropertyTypeAction, "add");
    Utils::setThemeIcon(mRemovePropertyTypeAction, "remove");
    Utils::setThemeIcon(mAddValueAction, "add");
    Utils::setThemeIcon(mRemoveValueAction, "remove");
    Utils::setThemeIcon(mAddMemberAction, "add");
    Utils::setThemeIcon(mRemoveMemberAction, "remove");

    auto stretch = new QWidget;
    stretch->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Minimum);

    QToolBar *importExportToolBar = createSmallToolBar(this);
    importExportToolBar->addWidget(stretch);
    importExportToolBar->addAction(mImportAction);
    importExportToolBar->addAction(mExportAction);
    mUi->layout->insertWidget(0, importExportToolBar);

    QToolBar *propertyTypesToolBar = createSmallToolBar(this);
    propertyTypesToolBar->addAction(mAddEnumPropertyTypeAction);
    propertyTypesToolBar->addAction(mAddClassPropertyTypeAction);
    propertyTypesToolBar->addAction(mRemovePropertyTypeAction);
    mUi->propertyTypesLayout->addWidget(propertyTypesToolBar);

    connect(mUi->propertyTypesView->selectionModel(), &QItemSelectionModel::selectionChanged,
            this, &PropertyTypesEditor::selectedPropertyTypesChanged);
    connect(mPropertyTypesModel, &PropertyTypesModel::modelReset,
            this, &PropertyTypesEditor::selectFirstPropertyType);

    connect(mAddEnumPropertyTypeAction, &QAction::triggered,
            this, [this] { addPropertyType(PropertyType::PT_Enum); });
    connect(mAddClassPropertyTypeAction, &QAction::triggered,
            this, [this] { addPropertyType(PropertyType::PT_Class); });
    connect(mRemovePropertyTypeAction, &QAction::triggered,
            this, &PropertyTypesEditor::removeSelectedPropertyType);

    connect(mAddValueAction, &QAction::triggered,
                this, &PropertyTypesEditor::addValue);
    connect(mRemoveValueAction, &QAction::triggered,
            this, &PropertyTypesEditor::removeValues);

    connect(mAddMemberAction, &QAction::triggered,
            this, &PropertyTypesEditor::openAddMemberDialog);
    connect(mRemoveMemberAction, &QAction::triggered,
            this, &PropertyTypesEditor::removeMember);
    connect(mRenameMemberAction, &QAction::triggered,
            this, &PropertyTypesEditor::renameMember);

    connect(mPropertyTypesModel, &PropertyTypesModel::nameChanged,
            this, &PropertyTypesEditor::propertyTypeNameChanged);
    connect(mPropertyTypesModel, &QAbstractItemModel::dataChanged,
            this, &PropertyTypesEditor::applyPropertyTypes);
    connect(mPropertyTypesModel, &QAbstractItemModel::rowsInserted,
            this, &PropertyTypesEditor::applyPropertyTypes);
    connect(mPropertyTypesModel, &QAbstractItemModel::rowsRemoved,
            this, &PropertyTypesEditor::applyPropertyTypes);

    connect(mValuesModel, &QAbstractItemModel::dataChanged,
            this, &PropertyTypesEditor::valuesChanged);
    connect(mValuesModel, &QAbstractItemModel::rowsInserted,
            this, &PropertyTypesEditor::valuesChanged);
    connect(mValuesModel, &QAbstractItemModel::rowsRemoved,
            this, &PropertyTypesEditor::valuesChanged);

    connect(mImportAction, &QAction::triggered,
            this, &PropertyTypesEditor::importPropertyTypes);
    connect(mExportAction, &QAction::triggered,
            this, &PropertyTypesEditor::exportPropertyTypes);

    Preferences *prefs = Preferences::instance();

    auto &project = ProjectManager::instance()->project();
    mPropertyTypesModel->setPropertyTypes(project.propertyTypes());

    connect(prefs, &Preferences::propertyTypesChanged,
            this, &PropertyTypesEditor::propertyTypesChanged);
    retranslateUi();
}

PropertyTypesEditor::~PropertyTypesEditor()
{
    delete mUi;
}

void PropertyTypesEditor::closeEvent(QCloseEvent *event)
{
    QWidget::closeEvent(event);
    if (event->isAccepted())
        emit closed();
}

void PropertyTypesEditor::changeEvent(QEvent *e)
{
    QWidget::changeEvent(e);
    switch (e->type()) {
    case QEvent::LanguageChange:
        mUi->retranslateUi(this);
        retranslateUi();
        break;
    default:
        break;
    }
}

void PropertyTypesEditor::retranslateUi()
{
    mAddEnumPropertyTypeAction->setText(tr("Add Enum"));
    mAddClassPropertyTypeAction->setText(tr("Add Class"));
    mRemovePropertyTypeAction->setText(tr("Remove Type"));

    mAddValueAction->setText(tr("Add Value"));
    mRemoveValueAction->setText(tr("Remove Value"));

    mAddMemberAction->setText(tr("Add Member"));
    mRemoveMemberAction->setText(tr("Remove Member"));
    mRenameMemberAction->setText(tr("Rename Member"));

    mExportAction->setText(tr("Export..."));
    mExportAction->setToolTip(tr("Export Property Types"));
    mImportAction->setText(tr("Import..."));
    mImportAction->setToolTip(tr("Import Property Types"));
}

void PropertyTypesEditor::addPropertyType(PropertyType::Type type)
{
    const QModelIndex newIndex = mPropertyTypesModel->addNewPropertyType(type);
    if (!newIndex.isValid())
        return;

    // Select and focus the new row and ensure it is visible
    QItemSelectionModel *sm = mUi->propertyTypesView->selectionModel();
    sm->select(newIndex,
               QItemSelectionModel::ClearAndSelect |
               QItemSelectionModel::Rows);
    sm->setCurrentIndex(newIndex, QItemSelectionModel::Current);
    mUi->propertyTypesView->edit(newIndex);
}

void PropertyTypesEditor::selectedPropertyTypesChanged()
{
    const QItemSelectionModel *sm = mUi->propertyTypesView->selectionModel();
    mRemovePropertyTypeAction->setEnabled(sm->hasSelection());
    updateDetails();
}

void PropertyTypesEditor::removeSelectedPropertyType()
{
    // Cancel potential editor first, since letting it apply can cause
    // reordering of the types in setData, which would cause the wrong types to
    // get removed.
    mUi->propertyTypesView->closePersistentEditor(mUi->propertyTypesView->currentIndex());

    const QModelIndex selectedIndex = selectedPropertyTypeIndex();
    const auto *propertyType = mPropertyTypesModel->propertyTypeAt(selectedIndex);
    if (!propertyType)
        return;

    if (!confirm(tr("Remove Type"),
                 tr("Are you sure you want to remove the type '%1'? This action cannot be undone.")
                 .arg(propertyType->name), this)) {
        return;
    }

    mPropertyTypesModel->removePropertyTypes({ selectedIndex });
}

/**
 * Returns the index of the currently selected property type, or an invalid
 * index if no or multiple property types are selected.
 */
QModelIndex PropertyTypesEditor::selectedPropertyTypeIndex() const
{
    const auto selectionModel = mUi->propertyTypesView->selectionModel();
    const QModelIndexList selectedRows = selectionModel->selectedRows();
    return selectedRows.size() == 1 ? selectedRows.first() : QModelIndex();
}

PropertyType *PropertyTypesEditor::selectedPropertyType() const
{
    return mPropertyTypesModel->propertyTypeAt(selectedPropertyTypeIndex());
}

void PropertyTypesEditor::currentMemberItemChanged(QtBrowserItem *item)
{
    mRemoveMemberAction->setEnabled(item);
    mRenameMemberAction->setEnabled(item);
}

void PropertyTypesEditor::propertyTypeNameChanged(const QModelIndex &index, const PropertyType &type)
{
    if (mSettingName)
        return;

    if (mNameEdit && index == selectedPropertyTypeIndex())
        mNameEdit->setText(type.name);
}

void PropertyTypesEditor::applyMemberToSelectedType(const QString &name, const QVariant &value)
{
    PropertyType *propertyType = selectedPropertyType();
    if (!propertyType || propertyType->type != PropertyType::PT_Class)
        return;

    auto &classType = static_cast<ClassPropertyType&>(*propertyType);
    classType.members.insert(name, value);

    applyPropertyTypes();
}

void PropertyTypesEditor::applyPropertyTypes()
{
    QScopedValueRollback<bool> settingPrefPropertyTypes(mSettingPrefPropertyTypes, true);
    emit Preferences::instance()->propertyTypesChanged();

    Project &project = ProjectManager::instance()->project();
    project.save();
}

void PropertyTypesEditor::propertyTypesChanged()
{
    // ignore signal if we caused it
    if (mSettingPrefPropertyTypes)
        return;

    auto &project = ProjectManager::instance()->project();
    mPropertyTypesModel->setPropertyTypes(project.propertyTypes());

    selectedPropertyTypesChanged();
}

void PropertyTypesEditor::setStorageType(EnumPropertyType::StorageType storageType)
{
    if (mUpdatingDetails)
        return;

    PropertyType *propertyType = selectedPropertyType();
    if (!propertyType || propertyType->type != PropertyType::PT_Enum)
        return;

    auto &enumType = static_cast<EnumPropertyType&>(*propertyType);
    if (enumType.storageType == storageType)
        return;

    enumType.storageType = storageType;
    applyPropertyTypes();
}

void PropertyTypesEditor::setValuesAsFlags(bool flags)
{
    if (mUpdatingDetails)
        return;

    PropertyType *propertyType = selectedPropertyType();
    if (!propertyType || propertyType->type != PropertyType::PT_Enum)
        return;

    auto &enumType = static_cast<EnumPropertyType&>(*propertyType);
    if (enumType.valuesAsFlags == flags)
        return;

    if (flags && !checkValueCount(enumType.values.count())) {
        mValuesAsFlagsCheckBox->setChecked(false);
        return;
    }

    enumType.valuesAsFlags = flags;
    applyPropertyTypes();
}

static QString nextValueText(const EnumPropertyType &propertyType)
{
    auto baseText = propertyType.name;
    if (!baseText.isEmpty())
        baseText.append(QLatin1Char('_'));

    // Search for a unique value, starting from the current count
    int number = propertyType.values.count();
    QString valueText;
    do {
        valueText = baseText + QString::number(number++);
    } while (propertyType.values.contains(valueText));

    return valueText;
}

void PropertyTypesEditor::addValue()
{
    const PropertyType *propertyType = selectedPropertyType();
    if (!propertyType || propertyType->type != PropertyType::PT_Enum)
        return;

    const auto &enumType = *static_cast<const EnumPropertyType*>(propertyType);
    const int row = mValuesModel->rowCount();

    if (enumType.valuesAsFlags && !checkValueCount(row + 1))
        return;

    if (!mValuesModel->insertRow(row))
        return;

    const QString valueText = nextValueText(enumType);

    const auto valueIndex = mValuesModel->index(row);
    mValuesView->setCurrentIndex(valueIndex);
    mValuesModel->setData(valueIndex, valueText, Qt::DisplayRole);
    mValuesView->edit(valueIndex);
}

void PropertyTypesEditor::removeValues()
{
    PropertyType *propertyType = selectedPropertyType();
    if (!propertyType || propertyType->type != PropertyType::PT_Enum)
        return;

    if (!confirm(tr("Remove Values"),
                 tr("Are you sure you want to remove the selected values from enum '%1'? This action cannot be undone.")
                 .arg(propertyType->name), this)) {
        return;
    }

    const QItemSelection selection = mValuesView->selectionModel()->selection();
    for (const QItemSelectionRange &range : selection)
        mValuesModel->removeRows(range.top(), range.height());
}

bool PropertyTypesEditor::checkValueCount(int count)
{
    if (count > 32) {
        QMessageBox::critical(this,
                              tr("Too Many Values"),
                              tr("Too many values for enum with values stored as flags. Maximum number of bit flags is 32."));
        return false;
    }
    return true;
}

void PropertyTypesEditor::openAddMemberDialog()
{
    const PropertyType *propertyType = selectedPropertyType();
    if (!propertyType || propertyType->type != PropertyType::PT_Class)
        return;

    AddPropertyDialog dialog(static_cast<const ClassPropertyType*>(propertyType), this);
    dialog.setWindowTitle(tr("Add Member"));

    if (dialog.exec() == AddPropertyDialog::Accepted)
        addMember(dialog.propertyName(), QVariant(dialog.propertyValue()));
}

void PropertyTypesEditor::addMember(const QString &name, const QVariant &value)
{
    if (name.isEmpty())
        return;

    PropertyType *propertyType = selectedPropertyType();
    if (!propertyType || propertyType->type != PropertyType::PT_Class)
        return;

    auto &classType = static_cast<ClassPropertyType&>(*propertyType);
    if (classType.members.contains(name)) {
        QMessageBox::critical(this,
                              tr("Error Adding Member"),
                              tr("There is already a member named '%1'.").arg(name));
        return;
    }

    applyMemberToSelectedType(name, value);
    updateDetails();
    editMember(name);
}

void PropertyTypesEditor::editMember(const QString &name)
{
    QtVariantProperty *property = mPropertiesHelper->property(name);
    if (!property)
        return;

    const QList<QtBrowserItem*> propertyItems = mMembersView->items(property);
    if (!propertyItems.isEmpty())
        mMembersView->editItem(propertyItems.first());
}

void PropertyTypesEditor::removeMember()
{
    QtBrowserItem *item = mMembersView->currentItem();
    if (!item)
        return;

    PropertyType *propertyType = selectedPropertyType();
    if (!propertyType || propertyType->type != PropertyType::PT_Class)
        return;

    const QString name = item->property()->propertyName();

    if (!confirm(tr("Remove Member"),
                 tr("Are you sure you want to remove '%1' from class '%2'? This action cannot be undone.")
                 .arg(name, propertyType->name), this)) {
        return;
    }

    // Select a different item before removing the current one
    QList<QtBrowserItem *> items = mMembersView->topLevelItems();
    if (items.count() > 1) {
        const int currentItemIndex = items.indexOf(item);
        if (item == items.last())
            mMembersView->setCurrentItem(items.at(currentItemIndex - 1));
        else
            mMembersView->setCurrentItem(items.at(currentItemIndex + 1));
    }

    mPropertiesHelper->deleteProperty(item->property());

    static_cast<ClassPropertyType&>(*propertyType).members.remove(name);

    applyPropertyTypes();
}

void PropertyTypesEditor::renameMember()
{
    QtBrowserItem *item = mMembersView->currentItem();
    if (!item)
        return;

    const QString oldName = item->property()->propertyName();

    QInputDialog *dialog = new QInputDialog(mMembersView);
    dialog->setAttribute(Qt::WA_DeleteOnClose);
    dialog->setInputMode(QInputDialog::TextInput);
    dialog->setLabelText(tr("Name:"));
    dialog->setTextValue(oldName);
    dialog->setWindowTitle(tr("Rename Member"));
    connect(dialog, &QInputDialog::textValueSelected, this, &PropertyTypesEditor::renameMemberTo);
    dialog->open();
}

void PropertyTypesEditor::renameMemberTo(const QString &name)
{
    if (name.isEmpty())
        return;

    QtBrowserItem *item = mMembersView->currentItem();
    if (!item)
        return;

    const QString oldName = item->property()->propertyName();
    if (oldName == name)
        return;

    auto propertyType = selectedPropertyType();
    if (propertyType->type != PropertyType::PT_Class)
        return;

    auto &classType = *static_cast<ClassPropertyType*>(propertyType);
    if (!classType.members.contains(oldName))
        return;

    if (classType.members.contains(name)) {
        QMessageBox::critical(this,
                              tr("Error Renaming Member"),
                              tr("There is already a member named '%1'.").arg(name));
        return;
    }

    classType.members.insert(name, classType.members.take(oldName));

    applyPropertyTypes();
    updateDetails();
}

void PropertyTypesEditor::importPropertyTypes()
{
    Session &session = Session::current();
    const QString lastPath = session.lastPath(Session::ObjectTypesFile);
    const QString fileName =
            QFileDialog::getOpenFileName(this, tr("Import Property Types"),
                                         lastPath,
                                         QCoreApplication::translate("File Types", "Property Types files (*.json)"));
    if (fileName.isEmpty())
        return;

    session.setLastPath(Session::ObjectTypesFile, fileName);

    QFile file(fileName);
    if (!file.open(QIODevice::ReadOnly | QIODevice::Text)) {
        const auto error = QCoreApplication::translate("File Errors", "Could not open file for reading.");
        QMessageBox::critical(this, tr("Error Reading Property Types"), error);
        return;
    }

    QJsonParseError jsonError;
    const QJsonDocument document = QJsonDocument::fromJson(file.readAll(), &jsonError);
    if (document.isNull()) {
        QMessageBox::critical(this, tr("Error Reading Property Types"),
                              Utils::Error::jsonParseError(jsonError));
        return;
    }

    PropertyTypes typesToImport;
    typesToImport.loadFromJson(document.array(), QFileInfo(fileName).path());

    if (typesToImport.count() > 0) {
        mPropertyTypesModel->importPropertyTypes(std::move(typesToImport));
        applyPropertyTypes();
    }
}

void PropertyTypesEditor::exportPropertyTypes()
{
    Session &session = Session::current();
    QString lastPath = session.lastPath(Session::ObjectTypesFile);

    if (!lastPath.endsWith(QLatin1String(".json")))
        lastPath.append(QStringLiteral("/propertytypes.json"));

    const QString fileName =
            QFileDialog::getSaveFileName(this, tr("Export Property Types"),
                                         lastPath,
                                         QCoreApplication::translate("File Types", "Property Types files (*.json)"));
    if (fileName.isEmpty())
        return;

    session.setLastPath(Session::ObjectTypesFile, fileName);

    SaveFile file(fileName);

    if (!file.open(QIODevice::WriteOnly | QIODevice::Text)) {
        const auto error = QCoreApplication::translate("File Errors", "Could not open file for writing.");
        QMessageBox::critical(this, tr("Error Writing Property Types"), error);
        return;
    }

    const auto types = mPropertyTypesModel->propertyTypes();
    file.device()->write(QJsonDocument(types->toJson()).toJson());

    if (!file.commit())
        QMessageBox::critical(this, tr("Error Writing Property Types"), file.errorString());
}

void PropertyTypesEditor::updateDetails()
{
    QScopedValueRollback<bool> updatingDetails(mUpdatingDetails, true);

    const PropertyType *propertyType = selectedPropertyType();
    if (!propertyType) {
        setCurrentPropertyType(PropertyType::PT_Invalid);
        return;
    }

    setCurrentPropertyType(propertyType->type);

    switch (propertyType->type) {
    case PropertyType::PT_Invalid:
        break;
    case PropertyType::PT_Class: {
        const auto &classType = *static_cast<const ClassPropertyType*>(propertyType);

        mPropertiesHelper->clear();

        QMapIterator<QString, QVariant> it(classType.members);
        while (it.hasNext()) {
            it.next();

            const QString &name = it.key();
            const QVariant &value = it.value();

            QtProperty *property = mPropertiesHelper->createProperty(name, value);
            mMembersView->addProperty(property);
        }
        break;
    }
    case PropertyType::PT_Enum: {
        const auto &enumType = *static_cast<const EnumPropertyType*>(propertyType);

        mStorageTypeComboBox->setCurrentIndex(enumType.storageType);
        mValuesAsFlagsCheckBox->setChecked(enumType.valuesAsFlags);
        mValuesModel->setStringList(enumType.values);

        selectedValuesChanged(mValuesView->selectionModel()->selection());
        break;
    }
    }

    mNameEdit->setText(propertyType->name);
}

void PropertyTypesEditor::selectedValuesChanged(const QItemSelection &selected)
{
    mRemoveValueAction->setEnabled(!selected.isEmpty());
}

void deleteAllFromLayout(QLayout *layout)
{
    for (int i = layout->count() - 1; i >= 0; --i) {
        QLayoutItem *item = layout->takeAt(i);
        delete item->widget();

        if (auto layout = item->layout())
            deleteAllFromLayout(layout);

        delete item;
    }
}

void PropertyTypesEditor::setCurrentPropertyType(PropertyType::Type type)
{
    if (mCurrentPropertyType == type)
        return;

    mCurrentPropertyType = type;

    delete mPropertiesHelper;
    mPropertiesHelper = nullptr;

    if (mDetailsLayout) {
        deleteAllFromLayout(mDetailsLayout);
        delete mDetailsLayout;
    }

    mDetailsLayout = new QFormLayout;
    mUi->horizontalLayout->addLayout(mDetailsLayout);

    mNameEdit = nullptr;
    mStorageTypeComboBox = nullptr;
    mValuesAsFlagsCheckBox = nullptr;
    mValuesView = nullptr;
    mMembersView = nullptr;

    mAddValueAction->setEnabled(type == PropertyType::PT_Enum);
    mAddMemberAction->setEnabled(type == PropertyType::PT_Class);

    if (type == PropertyType::PT_Invalid)
        return;

    mNameEdit = new QLineEdit(mUi->groupBox);
    mNameEdit->addAction(PropertyTypesModel::iconForPropertyType(type), QLineEdit::LeadingPosition);

    connect(mNameEdit, &QLineEdit::editingFinished,
            this, &PropertyTypesEditor::nameEditingFinished);

    mDetailsLayout->addRow(tr("Name"), mNameEdit);

    switch (type) {
    case PropertyType::PT_Invalid:
        break;
    case PropertyType::PT_Class: {
        mMembersView = new QtTreePropertyBrowser(this);
        mPropertiesHelper = new CustomPropertiesHelper(mMembersView, this);

        connect(mPropertiesHelper, &CustomPropertiesHelper::propertyValueChanged,
                this, &PropertyTypesEditor::memberValueChanged);

        connect(mMembersView, &QtTreePropertyBrowser::currentItemChanged,
                this, &PropertyTypesEditor::currentMemberItemChanged);

        QToolBar *membersToolBar = createSmallToolBar(mUi->groupBox);
        membersToolBar->addAction(mAddMemberAction);
        membersToolBar->addAction(mRemoveMemberAction);
        membersToolBar->addAction(mRenameMemberAction);

        auto membersWithToolBarLayout = new QVBoxLayout;
        membersWithToolBarLayout->setSpacing(0);
        membersWithToolBarLayout->setContentsMargins(0, 0, 0, 0);
        membersWithToolBarLayout->addWidget(mMembersView);
        membersWithToolBarLayout->addWidget(membersToolBar);

        mDetailsLayout->addRow(tr("Members"), membersWithToolBarLayout);
        break;
    }
    case PropertyType::PT_Enum: {
        mStorageTypeComboBox = new QComboBox(mUi->groupBox);
        mStorageTypeComboBox->addItems({ tr("String"), tr("Number") });

        connect(mStorageTypeComboBox, static_cast<void(QComboBox::*)(int)>(&QComboBox::currentIndexChanged),
                this, [this] (int index) { if (index != -1) setStorageType(static_cast<EnumPropertyType::StorageType>(index)); });

        mValuesAsFlagsCheckBox = new QCheckBox(tr("Allow multiple values (flags)"), mUi->groupBox);

        connect(mValuesAsFlagsCheckBox, &QCheckBox::toggled,
                this, [this] (bool checked) { setValuesAsFlags(checked); });

        mValuesView = new QTreeView(this);
        mValuesView->setRootIsDecorated(false);
        mValuesView->setUniformRowHeights(true);
        mValuesView->setHeaderHidden(true);
        mValuesView->setSelectionMode(QAbstractItemView::ExtendedSelection);
        mValuesView->setModel(mValuesModel);

        connect(mValuesView->selectionModel(), &QItemSelectionModel::selectionChanged,
                this, &PropertyTypesEditor::selectedValuesChanged);

        QToolBar *valuesToolBar = createSmallToolBar(mUi->groupBox);
        valuesToolBar->addAction(mAddValueAction);
        valuesToolBar->addAction(mRemoveValueAction);

        auto valuesWithToolBarLayout = new QVBoxLayout;
        valuesWithToolBarLayout->setSpacing(0);
        valuesWithToolBarLayout->setContentsMargins(0, 0, 0, 0);
        valuesWithToolBarLayout->addWidget(mValuesView);
        valuesWithToolBarLayout->addWidget(valuesToolBar);

        mDetailsLayout->addRow(tr("Save as"), mStorageTypeComboBox);
        mDetailsLayout->addRow(QString(), mValuesAsFlagsCheckBox);
        mDetailsLayout->addRow(tr("Values"), valuesWithToolBarLayout);
        break;
    }
    }
}

void PropertyTypesEditor::selectFirstPropertyType()
{
    const QModelIndex firstIndex = mPropertyTypesModel->index(0, 0);
    if (firstIndex.isValid()) {
        mUi->propertyTypesView->selectionModel()->select(firstIndex,
                                                         QItemSelectionModel::ClearAndSelect |
                                                         QItemSelectionModel::Rows);
    } else {
        // make sure the properties view is empty
        updateDetails();
    }
}

void PropertyTypesEditor::valuesChanged()
{
    if (mUpdatingDetails)
        return;

    PropertyType *propertyType = selectedPropertyType();
    if (!propertyType || propertyType->type != PropertyType::PT_Enum)
        return;

    const QStringList newValues = mValuesModel->stringList();
    auto &enumType = static_cast<EnumPropertyType&>(*propertyType);
    enumType.values = newValues;

    applyPropertyTypes();
}

void PropertyTypesEditor::nameEditingFinished()
{
    const auto index = selectedPropertyTypeIndex();
    if (!index.isValid())
        return;

    const auto name = mNameEdit->text();
    const auto type = mPropertyTypesModel->propertyTypeAt(index);

    QScopedValueRollback<bool> settingName(mSettingName, true);
    if (!mPropertyTypesModel->setPropertyTypeName(index.row(), name))
        mNameEdit->setText(type->name);
}

void PropertyTypesEditor::memberValueChanged(const QString &name, const QVariant &value)
{
    if (mUpdatingDetails)
        return;

    applyMemberToSelectedType(name, value);
}

} // namespace Tiled

#include "moc_propertytypeseditor.cpp"
