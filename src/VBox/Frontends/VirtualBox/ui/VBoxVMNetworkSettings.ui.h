/**
 *
 * VBox frontends: Qt GUI ("VirtualBox"):
 * "VM network settings" dialog UI include (Qt Designer)
 */

/*
 * Copyright (C) 2006-2007 Sun Microsystems, Inc.
 *
 * This file is part of VirtualBox Open Source Edition (OSE), as
 * available from http://www.virtualbox.org. This file is free software;
 * you can redistribute it and/or modify it under the terms of the GNU
 * General Public License (GPL) as published by the Free Software
 * Foundation, in version 2 as it comes in the "COPYING" file of the
 * VirtualBox OSE distribution. VirtualBox OSE is distributed in the
 * hope that it will be useful, but WITHOUT ANY WARRANTY of any kind.
 *
 * Please contact Sun Microsystems, Inc., 4150 Network Circle, Santa
 * Clara, CA 95054 USA or visit http://www.sun.com if you need
 * additional information or have any questions.
 */

/****************************************************************************
** ui.h extension file, included from the uic-generated form implementation.
**
** If you wish to add, delete or rename functions or slots use
** Qt Designer which will update this file, preserving your code. Create an
** init() function in place of a constructor, and a destroy() function in
** place of a destructor.
*****************************************************************************/

void VBoxVMNetworkSettings::init()
{
    cbAdapterType->insertItem (vboxGlobal().toString (KNetworkAdapterType_Am79C970A));
    cbAdapterType->insertItem (vboxGlobal().toString (KNetworkAdapterType_Am79C973));
#ifdef VBOX_WITH_E1000
    cbAdapterType->insertItem (vboxGlobal().toString (KNetworkAdapterType_I82540EM));
    cbAdapterType->insertItem (vboxGlobal().toString (KNetworkAdapterType_I82543GC));
#endif

    leMACAddress->setValidator (new QRegExpValidator
                                (QRegExp ("[0-9A-Fa-f][02468ACEace][0-9A-Fa-f]{10}"), this));

    cbNetworkAttachment->insertItem (vboxGlobal().toString (KNetworkAttachmentType_Null));
    cbNetworkAttachment->insertItem (vboxGlobal().toString (KNetworkAttachmentType_NAT));
    cbNetworkAttachment->insertItem (vboxGlobal().toString (KNetworkAttachmentType_HostInterface));
    cbNetworkAttachment->insertItem (vboxGlobal().toString (KNetworkAttachmentType_Internal));

#if defined Q_WS_X11 && !defined VBOX_WITH_NETFLT
    leTAPDescriptor->setValidator (new QIntValidator (-1, std::numeric_limits <LONG>::max(), this));
#else
    /* hide unavailable settings (TAP setup and terminate apps) */
    frmTAPSetupTerminate->setHidden (true);
    /* disable unused interface name UI */
    frmHostInterface_X11->setHidden (true);
#endif

#if defined Q_WS_WIN || defined VBOX_WITH_NETFLT
    /* disable unused interface name UI */
    grbTAP->setHidden (true);
    /* Make the line editor of the editable combobox read-only to disable
     * modifications by the user. The combobox itself remains "editable" to
     * allow for setting text missing in the drop-down list. */
    cbHostInterfaceName->lineEdit()->setReadOnly (true);

    connect (grbEnabled, SIGNAL (toggled (bool)),
             this, SLOT (grbEnabledToggled (bool)));
#else
    /* disable unused interface name UI */
    txHostInterface_WIN->setHidden (true);
    cbHostInterfaceName->setHidden (true);
    /* setup iconsets */
    pbTAPSetup->setIconSet (VBoxGlobal::iconSet ("select_file_16px.png",
                                                 "select_file_dis_16px.png"));
    pbTAPTerminate->setIconSet (VBoxGlobal::iconSet ("select_file_16px.png",
                                                     "select_file_dis_16px.png"));
#endif

    /* the TAP file descriptor setting is always invisible -- currently not used
     * (remove the relative code at all? -- just leave for some time...) */
    frmTAPDescriptor->setHidden (true);
}

VBoxVMNetworkSettings::CheckPageResult
VBoxVMNetworkSettings::checkPage (const QStringList &aList)
{
    KNetworkAttachmentType type =
        vboxGlobal().toNetworkAttachmentType (cbNetworkAttachment->currentText());
    if (!grbEnabled->isChecked())
        return CheckPage_Ok;
    else
#if defined Q_WS_WIN || defined VBOX_WITH_NETFLT
    if (type == KNetworkAttachmentType_HostInterface &&
        isInterfaceInvalid (aList, cbHostInterfaceName->currentText()))
        return CheckPage_InvalidInterface;
    else
#else
    NOREF (aList);
#endif
    if (type == KNetworkAttachmentType_Internal &&
        cbInternalNetworkName->currentText().isEmpty())
        return CheckPage_NoNetworkName;
    else
        return CheckPage_Ok;
}

void VBoxVMNetworkSettings::loadInterfaceList (const QStringList &aList,
                                               const QString &aNillItem)
{
#if defined Q_WS_WIN || defined VBOX_WITH_NETFLT
    /* save current list item name */
    QString currentListItemName = cbHostInterfaceName->currentText();
    /* clear current list */
    cbHostInterfaceName->clear();
    /* load current list items */
    if (aList.count())
        cbHostInterfaceName->insertStringList (aList);
    else
        cbHostInterfaceName->setCurrentText (aNillItem);

    if (currentListItemName.isEmpty() || currentListItemName == aNillItem)
        cbHostInterfaceName->setCurrentItem (0);
    else
        cbHostInterfaceName->setCurrentText (currentListItemName);
#else
    NOREF (aList);
    NOREF (aNillItem);
#endif
}

void VBoxVMNetworkSettings::loadNetworksList (const QStringList &aList)
{
    QString curText = cbInternalNetworkName->currentText();
    cbInternalNetworkName->clear();
    cbInternalNetworkName->clearEdit();
    cbInternalNetworkName->insertStringList (aList);
    cbInternalNetworkName->setCurrentText (curText);
}

void VBoxVMNetworkSettings::getFromAdapter (const CNetworkAdapter &adapter)
{
    cadapter = adapter;

    grbEnabled->setChecked (adapter.GetEnabled());

    cbAdapterType->setCurrentText (vboxGlobal().
        toString (adapter.GetAdapterType()));

    KNetworkAttachmentType type = adapter.GetAttachmentType();
    cbNetworkAttachment->setCurrentItem (0);
    for (int i = 0; i < cbNetworkAttachment->count(); i ++)
        if (vboxGlobal().toNetworkAttachmentType (cbNetworkAttachment->text (i)) == type)
        {
            cbNetworkAttachment->setCurrentItem (i);
            cbNetworkAttachment_activated (cbNetworkAttachment->currentText());
            break;
        }

    leMACAddress->setText (adapter.GetMACAddress());

    chbCableConnected->setChecked (adapter.GetCableConnected());

#if defined Q_WS_WIN || defined VBOX_WITH_NETFLT
    if (!adapter.GetHostInterface().isEmpty())
        cbHostInterfaceName->setCurrentText (adapter.GetHostInterface());
#else
    leHostInterface->setText (adapter.GetHostInterface());
#endif
    cbInternalNetworkName->setCurrentText (adapter.GetInternalNetwork());

#if defined Q_WS_X11 && !defined VBOX_WITH_NETFLT
    leTAPDescriptor->setText (QString::number (adapter.GetTAPFileDescriptor()));
    leTAPSetup->setText (adapter.GetTAPSetupApplication());
    leTAPTerminate->setText (adapter.GetTAPTerminateApplication());
#endif
}

void VBoxVMNetworkSettings::putBackToAdapter()
{
    cadapter.SetEnabled (grbEnabled->isChecked());

    cadapter.SetAdapterType (vboxGlobal().
        toNetworkAdapterType (cbAdapterType->currentText()));

    KNetworkAttachmentType type =
        vboxGlobal().toNetworkAttachmentType (cbNetworkAttachment->currentText());
    switch (type)
    {
        case KNetworkAttachmentType_Null:
            cadapter.Detach();
            break;
        case KNetworkAttachmentType_NAT:
            cadapter.AttachToNAT();
            break;
        case KNetworkAttachmentType_HostInterface:
            cadapter.AttachToHostInterface();
            break;
        case KNetworkAttachmentType_Internal:
            cadapter.AttachToInternalNetwork();
            break;
        default:
            AssertMsgFailed (("Invalid network attachment type: %d", type));
            break;
    }

    cadapter.SetMACAddress (leMACAddress->text());

    cadapter.SetCableConnected (chbCableConnected->isChecked());

    if (type == KNetworkAttachmentType_HostInterface)
    {
#if defined Q_WS_WIN || defined VBOX_WITH_NETFLT
        if (!cbHostInterfaceName->currentText().isEmpty())
            cadapter.SetHostInterface (cbHostInterfaceName->currentText());
#else
        QString iface = leHostInterface->text();
        cadapter.SetHostInterface (iface.isEmpty() ? QString::null : iface);
#endif
#if defined Q_WS_X11 && !defined VBOX_WITH_NETFLT
        cadapter.SetTAPFileDescriptor (leTAPDescriptor->text().toLong());
        QString setup = leTAPSetup->text();
        cadapter.SetTAPSetupApplication (setup.isEmpty() ? QString::null : setup);
        QString term = leTAPTerminate->text();
        cadapter.SetTAPTerminateApplication (term.isEmpty() ? QString::null : term);
#endif
    }
    else if (type == KNetworkAttachmentType_Internal)
        cadapter.SetInternalNetwork (cbInternalNetworkName->currentText());
}

void VBoxVMNetworkSettings::setValidator (QIWidgetValidator *aWalidator)
{
    mWalidator = aWalidator;
}

void VBoxVMNetworkSettings::revalidate()
{
    mWalidator->revalidate();
}

void VBoxVMNetworkSettings::grbEnabledToggled (bool aOn)
{
#if defined Q_WS_WIN || defined VBOX_WITH_NETFLT
    cbNetworkAttachment_activated (cbNetworkAttachment->currentText());
#else
    NOREF (aOn);
#endif
}

void VBoxVMNetworkSettings::cbNetworkAttachment_activated (const QString &aString)
{
    bool enableHostIf = grbEnabled->isChecked() &&
                        vboxGlobal().toNetworkAttachmentType (aString) ==
                        KNetworkAttachmentType_HostInterface;
    bool enableIntNet = grbEnabled->isChecked() &&
                        vboxGlobal().toNetworkAttachmentType (aString) ==
                        KNetworkAttachmentType_Internal;
#if defined Q_WS_WIN || defined VBOX_WITH_NETFLT
    txHostInterface_WIN->setEnabled (enableHostIf);
    cbHostInterfaceName->setEnabled (enableHostIf);
#else
    grbTAP->setEnabled (enableHostIf);
#endif
    txInternalNetwork->setEnabled (enableIntNet);
    cbInternalNetworkName->setEnabled (enableIntNet);
}

bool VBoxVMNetworkSettings::isInterfaceInvalid (const QStringList &aList,
                                                const QString &aIface)
{
#if defined Q_WS_WIN || defined VBOX_WITH_NETFLT
    return aList.find (aIface) == aList.end();
#else
    NOREF (aList);
    NOREF (aIface);
    return false;
#endif
}

void VBoxVMNetworkSettings::pbGenerateMAC_clicked()
{
    cadapter.SetMACAddress (QString::null);
    leMACAddress->setText (cadapter.GetMACAddress());
}

void VBoxVMNetworkSettings::pbTAPSetup_clicked()
{
    QString selected = QFileDialog::getOpenFileName (
        "/",
        QString::null,
        this,
        NULL,
        tr ("Select TAP setup application"));

    if (selected)
        leTAPSetup->setText (selected);
}

void VBoxVMNetworkSettings::pbTAPTerminate_clicked()
{
    QString selected = QFileDialog::getOpenFileName (
        "/",
        QString::null,
        this,
        NULL,
        tr ("Select TAP terminate application"));

    if (selected)
        leTAPTerminate->setText (selected);
}