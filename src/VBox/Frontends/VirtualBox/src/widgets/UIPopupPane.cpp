/* $Id: UIPopupPane.cpp $ */
/** @file
 *
 * VBox frontends: Qt GUI ("VirtualBox"):
 * UIPopupPane class implementation
 */

/*
 * Copyright (C) 2013 Oracle Corporation
 *
 * This file is part of VirtualBox Open Source Edition (OSE), as
 * available from http://www.virtualbox.org. This file is free software;
 * you can redistribute it and/or modify it under the terms of the GNU
 * General Public License (GPL) as published by the Free Software
 * Foundation, in version 2 as it comes in the "COPYING" file of the
 * VirtualBox OSE distribution. VirtualBox OSE is distributed in the
 * hope that it will be useful, but WITHOUT ANY WARRANTY of any kind.
 */

/* Qt includes: */
#include <QPainter>

/* GUI includes: */
#include "UIPopupPane.h"
#include "UIPopupPaneTextPane.h"
#include "UIPopupPaneButtonPane.h"
#include "UIAnimationFramework.h"
#include "QIMessageBox.h"

UIPopupPane::UIPopupPane(QWidget *pParent,
                         const QString &strMessage, const QString &strDetails,
                         const QMap<int, QString> &buttonDescriptions,
                         bool fProposeAutoConfirmation)
    : QIWithRetranslateUI<QWidget>(pParent)
    , m_fPolished(false)
    , m_iLayoutMargin(10), m_iLayoutSpacing(5)
    , m_strMessage(strMessage), m_strDetails(strDetails)
    , m_fProposeAutoConfirmation(fProposeAutoConfirmation)
    , m_buttonDescriptions(buttonDescriptions)
    , m_fShown(false)
    , m_pShowAnimation(0)
    , m_fCanLooseFocus(!m_buttonDescriptions.isEmpty())
    , m_fFocused(!m_fCanLooseFocus)
    , m_fHovered(m_fFocused)
    , m_iDefaultOpacity(180)
    , m_iHoveredOpacity(250)
    , m_iOpacity(m_fHovered ? m_iHoveredOpacity : m_iDefaultOpacity)
    , m_pTextPane(0), m_pButtonPane(0)
{
    /* Prepare: */
    prepare();
}

void UIPopupPane::recall()
{
    /* Close popup-pane with *escape* button: */
    done(m_pButtonPane->escapeButton());
}

void UIPopupPane::setMessage(const QString &strMessage)
{
    /* Make sure the message has changed: */
    if (m_strMessage == strMessage)
        return;

    /* Fetch new message: */
    m_strMessage = strMessage;
    m_pTextPane->setText(m_strMessage);
}

void UIPopupPane::setDetails(const QString &strDetails)
{
    /* Make sure the details has changed: */
    if (m_strDetails == strDetails)
        return;

    /* Fetch new details: */
    m_strDetails = strDetails;
}

void UIPopupPane::setProposeAutoConfirmation(bool fPropose)
{
    /* Make sure the auto-confirmation-proposal has changed: */
    if (m_fProposeAutoConfirmation == fPropose)
        return;

    /* Fetch new auto-confirmation-proposal: */
    m_fProposeAutoConfirmation = fPropose;
    m_pTextPane->setProposeAutoConfirmation(m_fProposeAutoConfirmation);
}

void UIPopupPane::setMinimumSizeHint(const QSize &minimumSizeHint)
{
    /* Make sure the size-hint has changed: */
    if (m_minimumSizeHint == minimumSizeHint)
        return;

    /* Fetch new size-hint: */
    m_minimumSizeHint = minimumSizeHint;

    /* Notify parent popup-stack: */
    emit sigSizeHintChanged();
}

void UIPopupPane::layoutContent()
{
    /* Variables: */
    const int iWidth = width();
    const int iHeight = height();
    const QSize buttonPaneMinimumSizeHint = m_pButtonPane->minimumSizeHint();
    const int iButtonPaneMinimumWidth = buttonPaneMinimumSizeHint.width();
    const int iButtonPaneMinimumHeight = buttonPaneMinimumSizeHint.height();
    const int iTextPaneWidth = iWidth - 2 * m_iLayoutMargin - m_iLayoutSpacing - iButtonPaneMinimumWidth;
    const int iTextPaneHeight = m_pTextPane->minimumSizeHint().height();
    const int iMaximumHeight = qMax(iTextPaneHeight, iButtonPaneMinimumHeight);
    const int iMinimumHeight = qMin(iTextPaneHeight, iButtonPaneMinimumHeight);
    const int iHeightShift = (iMaximumHeight - iMinimumHeight) / 2;
    const bool fTextPaneShifted = iTextPaneHeight < iButtonPaneMinimumHeight;

    /* Text-pane: */
    m_pTextPane->move(m_iLayoutMargin,
                      fTextPaneShifted ? m_iLayoutMargin + iHeightShift : m_iLayoutMargin);
    m_pTextPane->resize(iTextPaneWidth,
                        iTextPaneHeight);
    m_pTextPane->layoutContent();

    /* Button-pane: */
    m_pButtonPane->move(m_iLayoutMargin + iTextPaneWidth + m_iLayoutSpacing,
                        m_iLayoutMargin);
    m_pButtonPane->resize(iButtonPaneMinimumWidth,
                          iHeight - 2 * m_iLayoutMargin);
}

void UIPopupPane::sltMarkAsShown()
{
    /* Mark popup-pane as 'shown': */
    m_fShown = true;
}

void UIPopupPane::sltHandleProposalForWidth(int iWidth)
{
    /* Make sure text-pane exists: */
    if (!m_pTextPane)
        return;

    /* Subtract layout margins: */
    iWidth -= 2 * m_iLayoutMargin;
    /* Subtract layout spacing: */
    iWidth -= m_iLayoutSpacing;
    /* Subtract button-pane width: */
    iWidth -= m_pButtonPane->minimumSizeHint().width();

    /* Propose resulting width to text-pane: */
    emit sigProposeTextPaneWidth(iWidth);
}

void UIPopupPane::sltUpdateSizeHint()
{
    /* Calculate minimum width-hint: */
    int iMinimumWidthHint = 0;
    {
        /* Take into account layout: */
        iMinimumWidthHint += 2 * m_iLayoutMargin;
        {
            /* Take into account widgets: */
            iMinimumWidthHint += m_pTextPane->minimumSizeHint().width();
            iMinimumWidthHint += m_iLayoutSpacing;
            iMinimumWidthHint += m_pButtonPane->minimumSizeHint().width();
        }
    }

    /* Calculate minimum height-hint: */
    int iMinimumHeightHint = 0;
    {
        /* Take into account layout: */
        iMinimumHeightHint += 2 * m_iLayoutMargin;
        {
            /* Take into account widgets: */
            const int iTextPaneHeight = m_pTextPane->minimumSizeHint().height();
            const int iButtonBoxHeight = m_pButtonPane->minimumSizeHint().height();
            iMinimumHeightHint += qMax(iTextPaneHeight, iButtonBoxHeight);
        }
    }

    /* Compose minimum size-hints: */
    m_hiddenSizeHint = QSize(iMinimumWidthHint, 1);
    m_shownSizeHint = QSize(iMinimumWidthHint, iMinimumHeightHint);
    m_minimumSizeHint = m_fShown ? m_shownSizeHint : m_hiddenSizeHint;

    /* Update 'show/hide' animation: */
    if (m_pShowAnimation)
        m_pShowAnimation->update();

    /* Notify parent popup-stack: */
    emit sigSizeHintChanged();
}

void UIPopupPane::sltButtonClicked(int iButtonID)
{
    /* Complete popup with corresponding code: */
    done(iButtonID & AlertButtonMask);
}

void UIPopupPane::prepare()
{
    /* Prepare this: */
    installEventFilter(this);
    /* Prepare background: */
    prepareBackground();
    /* Prepare content: */
    prepareContent();
    /* Prepare animation: */
    prepareAnimation();

    /* Update size-hint: */
    sltUpdateSizeHint();
}

void UIPopupPane::prepareBackground()
{
    /* Prepare palette: */
    QPalette pal = palette();
    pal.setColor(QPalette::Window, QApplication::palette().color(QPalette::Window));
    setPalette(pal);
}

void UIPopupPane::prepareContent()
{
    /* Create message-label: */
    m_pTextPane = new UIPopupPaneTextPane(this, m_strMessage, m_fProposeAutoConfirmation, m_fFocused);
    {
        /* Prepare label: */
        connect(this, SIGNAL(sigProposeTextPaneWidth(int)), m_pTextPane, SLOT(sltHandleProposalForWidth(int)));
        connect(m_pTextPane, SIGNAL(sigSizeHintChanged()), this, SLOT(sltUpdateSizeHint()));
        m_pTextPane->installEventFilter(this);
    }

    /* Create button-box: */
    m_pButtonPane = new UIPopupPaneButtonPane(this);
    {
        /* Prepare button-box: */
        connect(m_pButtonPane, SIGNAL(sigButtonClicked(int)), this, SLOT(sltButtonClicked(int)));
        m_pButtonPane->installEventFilter(this);
        m_pButtonPane->setButtons(m_buttonDescriptions);
    }

    /* Prepare focus rules: */
    setFocusPolicy(Qt::StrongFocus);
    m_pTextPane->setFocusPolicy(Qt::StrongFocus);
    m_pButtonPane->setFocusPolicy(Qt::StrongFocus);
    setFocusProxy(m_pButtonPane);
    m_pTextPane->setFocusProxy(m_pButtonPane);

    /* Translate UI finally: */
    retranslateUi();
}

void UIPopupPane::prepareAnimation()
{
    /* Install 'show' animation for 'minimumSizeHint' property: */
    connect(this, SIGNAL(sigToShow()), this, SIGNAL(sigShow()), Qt::QueuedConnection);
    m_pShowAnimation = UIAnimation::installPropertyAnimation(this, "minimumSizeHint", "hiddenSizeHint", "shownSizeHint",
                                                             SIGNAL(sigShow()), SIGNAL(sigHide()));
    connect(m_pShowAnimation, SIGNAL(sigStateEnteredFinal()), this, SLOT(sltMarkAsShown()));

    /* Install 'hover' animation for 'opacity' property: */
    UIAnimation::installPropertyAnimation(this, "opacity", "defaultOpacity", "hoveredOpacity",
                                          SIGNAL(sigHoverEnter()), SIGNAL(sigHoverLeave()), m_fHovered);
}

void UIPopupPane::retranslateUi()
{
    /* Translate tool-tips: */
    retranslateToolTips();
}

void UIPopupPane::retranslateToolTips()
{
    /* Translate pane & text-pane tool-tips: */
    if (m_fFocused)
    {
        setToolTip(QString());
        m_pTextPane->setToolTip(QString());
    }
    else
    {
        setToolTip(QApplication::translate("UIPopupCenter", "Click for full details"));
        m_pTextPane->setToolTip(QApplication::translate("UIPopupCenter", "Click for full details"));
    }
}

bool UIPopupPane::eventFilter(QObject *pWatched, QEvent *pEvent)
{
    /* Depending on event-type: */
    switch (pEvent->type())
    {
        /* Something is hovered: */
        case QEvent::HoverEnter:
        case QEvent::Enter:
        {
            /* Hover pane if not yet hovered: */
            if (!m_fHovered)
            {
                m_fHovered = true;
                emit sigHoverEnter();
            }
            break;
        }
        /* Nothing is hovered: */
        case QEvent::Leave:
        {
            /* Unhover pane if hovered but not focused: */
            if (pWatched == this && m_fHovered && !m_fFocused)
            {
                m_fHovered = false;
                emit sigHoverLeave();
            }
            break;
        }
        /* Pane is clicked with mouse: */
        case QEvent::MouseButtonPress:
        {
            /* Focus pane if not focused: */
            if (!m_fFocused)
            {
                m_fFocused = true;
                emit sigFocusEnter();
                /* Hover pane if not hovered: */
                if (!m_fHovered)
                {
                    m_fHovered = true;
                    emit sigHoverEnter();
                }
                /* Translate tool-tips: */
                retranslateToolTips();
            }
            break;
        }
        /* Pane is unfocused: */
        case QEvent::FocusOut:
        {
            /* Unhocus pane if focused: */
            if (m_fCanLooseFocus && m_fFocused)
            {
                m_fFocused = false;
                emit sigFocusLeave();
                /* Unhover pane if hovered: */
                if (m_fHovered)
                {
                    m_fHovered = false;
                    emit sigHoverLeave();
                }
                /* Translate tool-tips: */
                retranslateToolTips();
            }
            break;
        }
        /* Default case: */
        default: break;
    }
    /* Do not filter anything: */
    return false;
}

void UIPopupPane::showEvent(QShowEvent *pEvent)
{
    /* Call to base-class: */
    QWidget::showEvent(pEvent);

    /* Polish border: */
    if (m_fPolished)
        return;
    m_fPolished = true;

    /* Call to polish event: */
    polishEvent(pEvent);
}

void UIPopupPane::polishEvent(QShowEvent*)
{
    /* Focus if marked as 'focused': */
    if (m_fFocused)
        setFocus();

    /* Emit signal to start *show* animation: */
    emit sigToShow();
}

void UIPopupPane::paintEvent(QPaintEvent*)
{
    /* Compose painting rectangle,
     * Shifts are required for the antialiasing support: */
    const QRect rect(1, 1, width() - 2, height() - 2);

    /* Create painter: */
    QPainter painter(this);
    painter.setRenderHint(QPainter::Antialiasing);

    /* Configure clipping: */
    configureClipping(rect, painter);

    /* Paint background: */
    paintBackground(rect, painter);

    /* Paint frame: */
    paintFrame(painter);
}

void UIPopupPane::configureClipping(const QRect &rect, QPainter &painter)
{
    /* Configure clipping: */
    QPainterPath path;
    int iDiameter = 6;
    QSizeF arcSize(2 * iDiameter, 2 * iDiameter);
    path.moveTo(rect.x() + iDiameter, rect.y());
    path.arcTo(QRectF(path.currentPosition(), arcSize).translated(-iDiameter, 0), 90, 90);
    path.lineTo(path.currentPosition().x(), rect.y() + rect.height() - iDiameter);
    path.arcTo(QRectF(path.currentPosition(), arcSize).translated(0, -iDiameter), 180, 90);
    path.lineTo(rect.x() + rect.width() - iDiameter, path.currentPosition().y());
    path.arcTo(QRectF(path.currentPosition(), arcSize).translated(-iDiameter, -2 * iDiameter), 270, 90);
    path.lineTo(path.currentPosition().x(), rect.y() + iDiameter);
    path.arcTo(QRectF(path.currentPosition(), arcSize).translated(-2 * iDiameter, -iDiameter), 0, 90);
    path.closeSubpath();
    painter.setClipPath(path);
}

void UIPopupPane::paintBackground(const QRect &rect, QPainter &painter)
{
    /* Paint background: */
    QColor currentColor(palette().color(QPalette::Window));
    QColor newColor1(currentColor.red(), currentColor.green(), currentColor.blue(), opacity());
    QColor newColor2 = newColor1.darker(115);
    QLinearGradient headerGradient(rect.topLeft(), rect.bottomLeft());
    headerGradient.setColorAt(0, newColor1);
    headerGradient.setColorAt(1, newColor2);
    painter.fillRect(rect, headerGradient);
}

void UIPopupPane::paintFrame(QPainter &painter)
{
    /* Paint frame: */
    QColor currentColor(palette().color(QPalette::Window).darker(150));
    QPainterPath path = painter.clipPath();
    painter.setClipping(false);
    painter.strokePath(path, currentColor);
}

void UIPopupPane::done(int iResultCode)
{
    /* Was the popup auto-confirmed? */
    if (m_pTextPane->isAutoConfirmed())
        iResultCode |= AlertOption_AutoConfirmed;

    /* Notify listeners: */
    emit sigDone(iResultCode);
}
