#include "control_center_window.hpp"
#include "control_center_layout.hpp"

#include <commctrl.h>

#include <algorithm>
#include <array>

namespace bafx::control_center
{
namespace
{

void moveControl(
    const HWND control,
    const int x,
    const int y,
    const int width,
    const int height) noexcept
{
    if (control != nullptr)
    {
        // Suppress intermediate paints while the sibling controls overlap.
        // layoutControls() redraws the complete parent and child tree after
        // every control has reached its final position.
        static_cast<void>(SetWindowPos(
            control,
            nullptr,
            x,
            y,
            width,
            height,
            SWP_NOACTIVATE
                | SWP_NOCOPYBITS
                | SWP_NOREDRAW
                | SWP_NOOWNERZORDER
                | SWP_NOZORDER));
    }
}

void setControlFont(const HWND control, const HFONT font) noexcept
{
    if (control != nullptr && font != nullptr)
    {
        static_cast<void>(SendMessageW(
            control,
            WM_SETFONT,
            reinterpret_cast<WPARAM>(font),
            TRUE));
    }
}

}

void ControlCenterWindow::createFonts()
{
    const HFONT oldNormal = normalFont_;
    const HFONT oldTitle = titleFont_;
    const HFONT oldSection = sectionFont_;

    const HFONT newNormal = CreateFontW(
        -MulDiv(10, static_cast<int>(layoutDpi_), 72),
        0,
        0,
        0,
        FW_NORMAL,
        FALSE,
        FALSE,
        FALSE,
        DEFAULT_CHARSET,
        OUT_DEFAULT_PRECIS,
        CLIP_DEFAULT_PRECIS,
        CLEARTYPE_QUALITY,
        DEFAULT_PITCH | FF_DONTCARE,
        L"Segoe UI");
    const HFONT newTitle = CreateFontW(
        -MulDiv(20, static_cast<int>(layoutDpi_), 72),
        0,
        0,
        0,
        FW_SEMIBOLD,
        FALSE,
        FALSE,
        FALSE,
        DEFAULT_CHARSET,
        OUT_DEFAULT_PRECIS,
        CLIP_DEFAULT_PRECIS,
        CLEARTYPE_QUALITY,
        DEFAULT_PITCH | FF_DONTCARE,
        L"Segoe UI");
    const HFONT newSection = CreateFontW(
        -MulDiv(11, static_cast<int>(layoutDpi_), 72),
        0,
        0,
        0,
        FW_SEMIBOLD,
        FALSE,
        FALSE,
        FALSE,
        DEFAULT_CHARSET,
        OUT_DEFAULT_PRECIS,
        CLIP_DEFAULT_PRECIS,
        CLEARTYPE_QUALITY,
        DEFAULT_PITCH | FF_DONTCARE,
        L"Segoe UI");

    if (newNormal == nullptr || newTitle == nullptr || newSection == nullptr)
    {
        if (newNormal != nullptr)
        {
            DeleteObject(newNormal);
        }
        if (newTitle != nullptr)
        {
            DeleteObject(newTitle);
        }
        if (newSection != nullptr)
        {
            DeleteObject(newSection);
        }
        // Retain the currently selected fonts. Deleting a font that remains
        // selected in child controls would leave GDI with a dangling handle.
        return;
    }

    normalFont_ = newNormal;
    titleFont_ = newTitle;
    sectionFont_ = newSection;
    applyFonts();
    applyDpiMetrics();
    if (oldNormal != nullptr)
    {
        DeleteObject(oldNormal);
    }
    if (oldTitle != nullptr)
    {
        DeleteObject(oldTitle);
    }
    if (oldSection != nullptr)
    {
        DeleteObject(oldSection);
    }
}

void ControlCenterWindow::destroyFonts() noexcept
{
    if (normalFont_ != nullptr)
    {
        DeleteObject(normalFont_);
        normalFont_ = nullptr;
    }
    if (titleFont_ != nullptr)
    {
        DeleteObject(titleFont_);
        titleFont_ = nullptr;
    }
    if (sectionFont_ != nullptr)
    {
        DeleteObject(sectionFont_);
        sectionFont_ = nullptr;
    }
}

void ControlCenterWindow::applyFonts() const noexcept
{
    for (const HWND control : hotkeyControls_)
    {
        setControlFont(control, normalFont_);
    }
    setControlFont(hotkeysPageButton_, normalFont_);
    const std::array normalControls{
        statusText_,
        messageText_,
        basicPageButton_,
        advancedPageButton_,
        displayPageButton_,
        systemPageButton_,
        effectsEnabled_,
        effectsModeLabel_,
        effectsMode_,
        clickEnabled_,
        trailEnabled_,
        diskLayerEnabled_,
        ringsLayerEnabled_,
        clickShardsLayerEnabled_,
        trailShardsLayerEnabled_,
        trailLayerEnabled_,
        bloomLayerEnabled_,
        trailAlwaysOn_,
        leftClickEnabled_,
        rightClickEnabled_,
        middleClickEnabled_,
        globalScale_.label,
        globalScale_.trackbar,
        globalScale_.valueText,
        trailLength_.label,
        trailLength_.trackbar,
        trailLength_.valueText,
        trailWidth_.label,
        trailWidth_.trackbar,
        trailWidth_.valueText,
        inputSamplingRate_.label,
        inputSamplingRate_.trackbar,
        inputSamplingRate_.valueText,
        bloomIntensity_.label,
        bloomIntensity_.trackbar,
        bloomIntensity_.valueText,
        opacity_.label,
        opacity_.trackbar,
        opacity_.valueText,
        clickTimeScale_.label,
        clickTimeScale_.trackbar,
        clickTimeScale_.valueText,
        trailTimeScale_.label,
        trailTimeScale_.trackbar,
        trailTimeScale_.valueText,
        trailLifetimeMs_.label,
        trailLifetimeMs_.trackbar,
        trailLifetimeMs_.valueText,
        bloomDiffusion_.label,
        bloomDiffusion_.trackbar,
        bloomDiffusion_.valueText,
        bloomThreshold_.label,
        bloomThreshold_.trackbar,
        bloomThreshold_.valueText,
        bloomSoftKnee_.label,
        bloomSoftKnee_.trackbar,
        bloomSoftKnee_.valueText,
        bloomClamp_.label,
        bloomClamp_.trackbar,
        bloomClamp_.valueText,
        diskRadius_.label,
        diskRadius_.trackbar,
        diskRadius_.valueText,
        diskLifetimeMs_.label,
        diskLifetimeMs_.trackbar,
        diskLifetimeMs_.valueText,
        ringsHdrIntensity_.label,
        ringsHdrIntensity_.trackbar,
        ringsHdrIntensity_.valueText,
        ringsCount_.label,
        ringsCount_.trackbar,
        ringsCount_.valueText,
        ringsLifetimeMs_.label,
        ringsLifetimeMs_.trackbar,
        ringsLifetimeMs_.valueText,
        ringsRadiusMin_.label,
        ringsRadiusMin_.trackbar,
        ringsRadiusMin_.valueText,
        ringsRadiusMax_.label,
        ringsRadiusMax_.trackbar,
        ringsRadiusMax_.valueText,
        ringsAngularVelocityMultiplier_.label,
        ringsAngularVelocityMultiplier_.trackbar,
        ringsAngularVelocityMultiplier_.valueText,
        ringsRotationDirection_.label,
        ringsRotationDirection_.trackbar,
        ringsRotationDirection_.valueText,
        shardsHdrIntensity_.label,
        shardsHdrIntensity_.trackbar,
        shardsHdrIntensity_.valueText,
        shardsClickCount_.label,
        shardsClickCount_.trackbar,
        shardsClickCount_.valueText,
        shardsClickLifetimeMinMs_.label,
        shardsClickLifetimeMinMs_.trackbar,
        shardsClickLifetimeMinMs_.valueText,
        shardsClickLifetimeMaxMs_.label,
        shardsClickLifetimeMaxMs_.trackbar,
        shardsClickLifetimeMaxMs_.valueText,
        shardsClickRadius_.label,
        shardsClickRadius_.trackbar,
        shardsClickRadius_.valueText,
        shardsClickSpeedMin_.label,
        shardsClickSpeedMin_.trackbar,
        shardsClickSpeedMin_.valueText,
        shardsClickSpeedMax_.label,
        shardsClickSpeedMax_.trackbar,
        shardsClickSpeedMax_.valueText,
        shardsSizeMin_.label,
        shardsSizeMin_.trackbar,
        shardsSizeMin_.valueText,
        shardsSizeMax_.label,
        shardsSizeMax_.trackbar,
        shardsSizeMax_.valueText,
        trailOpacity_.label,
        trailOpacity_.trackbar,
        trailOpacity_.valueText,
        themeColorLabel_,
        themeColorEdit_,
        themeColorPreview_,
        themeColorChoose_,
        bloomQualityLabel_,
        bloomQuality_,
        backgroundModeLabel_,
        backgroundMode_,
        cursorExcluded_,
        allowSystemBorder_,
        idleOptimization_,
        fxProfileLabel_,
        fxProfileSelector_,
        fxProfileNameEdit_,
        applyFxProfileButton_,
        saveFxProfileButton_,
        deleteFxProfileButton_,
        startWithWindows_,
        startMinimized_,
        closeToTray_,
        controlCenterVersionText_,
        hostVersionText_,
        installStateText_,
        latestVersionText_,
        checkForUpdatesButton_,
        openReleaseButton_,
        repositoryStarHint_,
        openRepositoryButton_,
#if defined(BAFX_ENABLE_SPOUT2)
        spout2Enabled_,
        spout2SenderStatus_,
        obsSpoutPluginStatus_,
        spout2ObsHint_,
        refreshObsSpoutPluginButton_,
        openObsSpoutPluginPageButton_,
#endif
        displaySelectorLabel_,
        displaySelector_,
        displaySummaryText_,
        hdrEnabled_,
        activeFxRoiEnabled_,
        framePacingLabel_,
        framePacing_,
        displayIndependent_,
        displayEffectsEnabled_,
        displayHdrEnabled_,
        displayFramePacingLabel_,
        displayFramePacing_,
        displayDetailsText_,
        activeFxRoiDetailsText_,
        pauseButton_,
        refreshButton_,
        hostLifecycleButton_,
        openLogDirectoryButton_,
        clearLogsButton_,
        resetDefaultsButton_};
    for (const HWND control : normalControls)
    {
        setControlFont(control, normalFont_);
    }
    setControlFont(titleText_, titleFont_);
    setControlFont(effectsHeading_, sectionFont_);
    setControlFont(backgroundHeading_, sectionFont_);
    setControlFont(systemSettingsHeading_, sectionFont_);
    setControlFont(languageLabel_, normalFont_);
    setControlFont(languageSelector_, normalFont_);
    setControlFont(versionUpdateHeading_, sectionFont_);
    setControlFont(advancedTimingHeading_, sectionFont_);
    setControlFont(advancedParticlesHeading_, sectionFont_);
    setControlFont(advancedRingsHeading_, sectionFont_);
    setControlFont(advancedClickShardsHeading_, sectionFont_);
    setControlFont(advancedBloomHeading_, sectionFont_);
    setControlFont(advancedLayersHeading_, sectionFont_);
    setControlFont(displaySettingsHeading_, sectionFont_);
    setControlFont(displayDetailsHeading_, sectionFont_);
    setControlFont(activeFxRoiDetailsHeading_, sectionFont_);
    setControlFont(advancedTimingSectionButton_, normalFont_);
    setControlFont(advancedParticlesSectionButton_, normalFont_);
    setControlFont(advancedRingsSectionButton_, normalFont_);
    setControlFont(advancedClickShardsSectionButton_, normalFont_);
    setControlFont(advancedBloomSectionButton_, normalFont_);
    setControlFont(advancedLayersSectionButton_, normalFont_);
}

void ControlCenterWindow::applyDpiMetrics() const noexcept
{
    SendMessageW(backgroundMode_, CB_SETDROPPEDWIDTH, static_cast<WPARAM>(scale(470)), 0);
    SendMessageW(effectsMode_, CB_SETDROPPEDWIDTH, static_cast<WPARAM>(scale(320)), 0);
    const std::array comboBoxes{
        languageSelector_,
        bloomQuality_,
        effectsMode_,
        backgroundMode_,
        fxProfileSelector_,
        displaySelector_,
        framePacing_,
        displayFramePacing_};
    for (const HWND comboBox : comboBoxes)
    {
        if (comboBox == nullptr)
        {
            continue;
        }
        // A custom font does not automatically update the native combo-box
        // item height after WM_DPICHANGED. Explicit metrics prevent clipped
        // Chinese glyphs on 125%-200% scale factors.
        static_cast<void>(SendMessageW(
            comboBox,
            CB_SETITEMHEIGHT,
            static_cast<WPARAM>(-1),
            scale(26)));
        static_cast<void>(SendMessageW(comboBox, CB_SETITEMHEIGHT, 0U, scale(26)));
    }
    if (fxProfileSelector_ != nullptr)
    {
        // The collapsed selector stays compact; the drop-down is wider so
        // distinct long Profile names remain identifiable.
        static_cast<void>(SendMessageW(
            fxProfileSelector_,
            CB_SETDROPPEDWIDTH,
            scale(320),
            0));
    }
}

void ControlCenterWindow::layoutControls(
    const int clientWidth,
    const int clientHeight) const noexcept
{
    if (titleText_ == nullptr || clientWidth <= 0 || clientHeight <= 0)
    {
        return;
    }

    // Keep all sibling moves paint-free while their overlapping rectangles are
    // changing. The final redraw below is the single committed frame for the
    // complete layout, including the parent background.

    const int margin = scale(24);
    const int columnGap = scale(24);
    const int availableRightWidth = (std::clamp)(
        clientWidth / 3,
        scale(280),
        scale(340));
    const int leftWidth = clientWidth
        - margin * 2
        - columnGap
        - availableRightWidth;
    const int rightX = margin + leftWidth + columnGap;

    moveControl(titleText_, margin, scale(16), clientWidth - margin * 2, scale(36));
    moveControl(statusText_, margin, scale(54), clientWidth - margin * 2, scale(24));
    // setInfo() writes a title and detail on separate lines. Reserve both lines
    // while keeping the page-tab band fixed across DPI transitions.
    const int messageHeight = scale(36);
    moveControl(messageText_, margin, scale(82), clientWidth - margin * 2, messageHeight);

    const int contentTop = scale(150);
    const int tabWidth = (clientWidth - margin * 2 - scale(32)) / 5;
    const int tabGap = scale(8);
    moveControl(
        basicPageButton_,
        margin,
        scale(120),
        tabWidth,
        scale(30));
    moveControl(
        advancedPageButton_,
        margin + tabWidth + tabGap,
        scale(120),
        tabWidth,
        scale(30));
    moveControl(
        displayPageButton_,
        margin + (tabWidth + tabGap) * 2,
        scale(120),
        tabWidth,
        scale(30));
    moveControl(
        hotkeysPageButton_,
        margin + (tabWidth + tabGap) * 3,
        scale(120),
        tabWidth,
        scale(30));
    moveControl(systemPageButton_, margin + (tabWidth + tabGap) * 4,
        scale(120), tabWidth, scale(30));

    if (activePage_ == Page::Hotkeys)
    {
        layoutHotkeyControls(clientWidth, clientHeight);
        redrawWindowTree();
        return;
    }

    if (activePage_ == Page::DisplayPerformance)
    {
        const int actionHeight = scale(38);
        const int actionGap = scale(10);
        const int actionY = (std::max)(
            contentTop + scale(300),
            clientHeight - margin - actionHeight);
        const int panelHeight = (std::max)(
            scale(300),
            actionY - contentTop - scale(12));
        const int settingsWidth = (std::clamp)(
            clientWidth / 3,
            scale(300),
            scale(360));
        const int detailsX = margin + settingsWidth + columnGap;
        const int detailsWidth = (std::max)(
            scale(1),
            clientWidth - detailsX - margin);
        const int inset = scale(16);
        const int settingsContentX = margin + inset;
        const int settingsContentWidth = (std::max)(
            scale(1),
            settingsWidth - inset * 2);

        moveControl(
            displaySettingsHeading_,
            margin,
            contentTop,
            settingsWidth,
            panelHeight);
        moveControl(
            displaySelectorLabel_,
            settingsContentX,
            contentTop + scale(30),
            settingsContentWidth,
            scale(22));
        moveControl(
            displaySelector_,
            settingsContentX,
            contentTop + scale(52),
            settingsContentWidth,
            scale(34));
        moveControl(
            displaySummaryText_,
            settingsContentX,
            contentTop + scale(94),
            settingsContentWidth,
            scale(48));
        moveControl(
            hdrEnabled_,
            settingsContentX,
            contentTop + scale(148),
            settingsContentWidth,
            scale(30));
        moveControl(
            activeFxRoiEnabled_,
            settingsContentX,
            contentTop + scale(184),
            settingsContentWidth,
            scale(30));
        moveControl(
            framePacingLabel_,
            settingsContentX,
            contentTop + scale(220),
            settingsContentWidth,
            scale(22));
        moveControl(
            framePacing_,
            settingsContentX,
            contentTop + scale(242),
            settingsContentWidth,
            scale(34));

        const int detailsPanelHeight = (std::clamp)(
            panelHeight / 2,
            scale(174),
            scale(220));
        const int roiPanelGap = scale(10);
        const int roiPanelY = contentTop
            + detailsPanelHeight
            + roiPanelGap;
        const int roiPanelHeight = (std::max)(
            scale(1),
            panelHeight - detailsPanelHeight - roiPanelGap);
        moveControl(
            displayDetailsHeading_,
            detailsX,
            contentTop,
            detailsWidth,
            detailsPanelHeight);
        const int detailsContentX = detailsX + inset;
        const int detailsContentWidth = (std::max)(
            scale(1),
            detailsWidth - inset * 2);
        const int policyColumnGap = scale(12);
        const int policyColumnWidth = (std::max)(
            scale(1),
            (detailsContentWidth - policyColumnGap) / 2);
        moveControl(
            displayIndependent_,
            detailsContentX,
            contentTop + scale(28),
            policyColumnWidth,
            scale(30));
        moveControl(
            displayEffectsEnabled_,
            detailsContentX + policyColumnWidth + policyColumnGap,
            contentTop + scale(28),
            policyColumnWidth,
            scale(30));
        moveControl(
            displayHdrEnabled_,
            detailsContentX,
            contentTop + scale(62),
            policyColumnWidth,
            scale(30));
        moveControl(
            displayFramePacingLabel_,
            detailsContentX + policyColumnWidth + policyColumnGap,
            contentTop + scale(62),
            policyColumnWidth,
            scale(22));
        moveControl(
            displayFramePacing_,
            detailsContentX + policyColumnWidth + policyColumnGap,
            contentTop + scale(84),
            policyColumnWidth,
            scale(34));
        moveControl(
            displayDetailsText_,
            detailsContentX,
            contentTop + scale(124),
            detailsContentWidth,
            (std::max)(scale(1), detailsPanelHeight - scale(140)));
        moveControl(
            activeFxRoiDetailsHeading_,
            detailsX,
            roiPanelY,
            detailsWidth,
            roiPanelHeight);
        moveControl(
            activeFxRoiDetailsText_,
            detailsContentX,
            roiPanelY + scale(28),
            detailsContentWidth,
            (std::max)(scale(1), roiPanelHeight - scale(44)));

        const int actionWidth = (clientWidth - margin * 2 - actionGap * 3) / 4;
        moveControl(
            pauseButton_,
            margin,
            actionY,
            actionWidth,
            actionHeight);
        moveControl(
            refreshButton_,
            margin + actionWidth + actionGap,
            actionY,
            actionWidth,
            actionHeight);
        moveControl(
            hostLifecycleButton_,
            margin + (actionWidth + actionGap) * 2,
            actionY,
            actionWidth,
            actionHeight);
        moveControl(
            resetDefaultsButton_,
            margin + (actionWidth + actionGap) * 3,
            actionY,
            actionWidth,
            actionHeight);
        redrawWindowTree();
        return;
    }

    if (activePage_ == Page::System)
    {
        const int actionHeight = scale(38);
        const int actionGap = scale(10);
#if defined(BAFX_ENABLE_SPOUT2)
        constexpr int minimumSystemContentHeight = 356;
        constexpr int minimumSystemPanelHeight = 324;
#else
        constexpr int minimumSystemContentHeight = 336;
        constexpr int minimumSystemPanelHeight = 324;
#endif
        const int actionY = (std::max)(
            contentTop + scale(minimumSystemContentHeight),
            clientHeight - margin - actionHeight);
        const int panelHeight = (std::max)(
            scale(minimumSystemPanelHeight),
            actionY - contentTop - scale(12));
        const int panelWidth = clientWidth - margin * 2;
        const int panelGap = scale(16);
        const int updatePanelWidth = (std::clamp)(
            panelWidth * 2 / 5,
            scale(300),
            scale(340));
        const int systemPanelWidth = (std::max)(
            scale(1),
            panelWidth - panelGap - updatePanelWidth);
        const int updatePanelX = margin + systemPanelWidth + panelGap;
        const int inset = scale(16);
        const int contentX = margin + inset;
        const int contentWidth = (std::max)(
            scale(1),
            systemPanelWidth - inset * 2);

        moveControl(
            systemSettingsHeading_,
            margin,
            contentTop,
            systemPanelWidth,
            panelHeight);
        moveControl(languageLabel_, contentX, contentTop + scale(32), scale(108), scale(26));
        moveControl(languageSelector_, contentX + scale(116), contentTop + scale(28),
            contentWidth - scale(116), scale(160));
        moveControl(
            startWithWindows_,
            contentX,
            contentTop + scale(64),
            contentWidth,
            scale(28));
        moveControl(
            startMinimized_,
            contentX,
            contentTop + scale(92),
            contentWidth,
            scale(28));
        moveControl(
            closeToTray_,
            contentX,
            contentTop + scale(120),
            contentWidth,
            scale(28));
#if defined(BAFX_ENABLE_SPOUT2)
        moveControl(
            spout2Enabled_,
            contentX,
            contentTop + scale(148),
            contentWidth,
            scale(28));
        moveControl(
            spout2SenderStatus_,
            contentX,
            contentTop + scale(180),
            contentWidth,
            scale(56));
        moveControl(
            obsSpoutPluginStatus_,
            contentX,
            contentTop + scale(240),
            contentWidth,
            scale(42));
        moveControl(
            spout2ObsHint_,
            contentX,
            contentTop + scale(284),
            contentWidth,
            scale(36));
        const int obsButtonGap = scale(10);
        const int obsButtonWidth = (contentWidth - obsButtonGap) / 2;
        moveControl(
            refreshObsSpoutPluginButton_,
            contentX,
            contentTop + scale(328),
            obsButtonWidth,
            scale(30));
        moveControl(
            openObsSpoutPluginPageButton_,
            contentX + obsButtonWidth + obsButtonGap,
            contentTop + scale(328),
            obsButtonWidth,
            scale(30));
#endif

        moveControl(
            versionUpdateHeading_,
            updatePanelX,
            contentTop,
            updatePanelWidth,
            panelHeight);
        const int updateContentX = updatePanelX + inset;
        const int updateContentWidth = (std::max)(
            scale(1),
            updatePanelWidth - inset * 2);
        moveControl(
            controlCenterVersionText_,
            updateContentX,
            contentTop + scale(36),
            updateContentWidth,
            scale(24));
        moveControl(
            hostVersionText_,
            updateContentX,
            contentTop + scale(68),
            updateContentWidth,
            scale(24));
        moveControl(
            installStateText_,
            updateContentX,
            contentTop + scale(100),
            updateContentWidth,
            scale(44));
        moveControl(
            latestVersionText_,
            updateContentX,
            contentTop + scale(152),
            updateContentWidth,
            scale(40));
        const int updateButtonGap = scale(10);
        const int updateButtonWidth = (std::max)(
            scale(1),
            (updateContentWidth - updateButtonGap) / 2);
        moveControl(
            checkForUpdatesButton_,
            updateContentX,
            contentTop + scale(204),
            updateButtonWidth,
            scale(32));
        moveControl(
            openReleaseButton_,
            updateContentX + updateButtonWidth + updateButtonGap,
            contentTop + scale(204),
            updateButtonWidth,
            scale(32));
        moveControl(
            repositoryStarHint_,
            updateContentX,
            contentTop + scale(248),
            updateContentWidth,
            scale(24));
        moveControl(
            openRepositoryButton_,
            updateContentX,
            contentTop + scale(280),
            updateContentWidth,
            scale(32));

        moveControl(openLogDirectoryButton_, updateContentX, contentTop + scale(316),
            updateButtonWidth, scale(30));
        moveControl(clearLogsButton_, updateContentX + updateButtonWidth + updateButtonGap,
            contentTop + scale(316), updateButtonWidth, scale(30));

        const int actionWidth = (clientWidth - margin * 2 - actionGap * 3) / 4;
        moveControl(
            pauseButton_,
            margin,
            actionY,
            actionWidth,
            actionHeight);
        moveControl(
            refreshButton_,
            margin + actionWidth + actionGap,
            actionY,
            actionWidth,
            actionHeight);
        moveControl(
            hostLifecycleButton_,
            margin + (actionWidth + actionGap) * 2,
            actionY,
            actionWidth,
            actionHeight);
        moveControl(
            resetDefaultsButton_,
            margin + (actionWidth + actionGap) * 3,
            actionY,
            actionWidth,
            actionHeight);
        redrawWindowTree();
        return;
    }

    if (activePage_ == Page::Advanced)
    {
        const int sectionButtonGap = scale(8);
        constexpr int sectionButtonCount = 6;
        const int sectionButtonWidth = (std::max)(
            scale(1),
            (clientWidth - margin * 2
                - sectionButtonGap * (sectionButtonCount - 1))
                / sectionButtonCount);
        moveControl(
            advancedTimingSectionButton_,
            margin,
            contentTop,
            sectionButtonWidth,
            scale(30));
        moveControl(
            advancedParticlesSectionButton_,
            margin + sectionButtonWidth + sectionButtonGap,
            contentTop,
            sectionButtonWidth,
            scale(30));
        moveControl(
            advancedRingsSectionButton_,
            margin + (sectionButtonWidth + sectionButtonGap) * 2,
            contentTop,
            sectionButtonWidth,
            scale(30));
        moveControl(
            advancedClickShardsSectionButton_,
            margin + (sectionButtonWidth + sectionButtonGap) * 3,
            contentTop,
            sectionButtonWidth,
            scale(30));
        moveControl(
            advancedBloomSectionButton_,
            margin + (sectionButtonWidth + sectionButtonGap) * 4,
            contentTop,
            sectionButtonWidth,
            scale(30));
        moveControl(
            advancedLayersSectionButton_,
            margin + (sectionButtonWidth + sectionButtonGap) * 5,
            contentTop,
            sectionButtonWidth,
            scale(30));

        const int panelTop = contentTop + scale(38);
        const int actionHeight = scale(38);
        const int actionGap = scale(10);
        const int actionY = (std::max)(
            panelTop + scale(262),
            clientHeight - margin - actionHeight);
        const int groupHeight = (std::max)(
            scale(262),
            actionY - panelTop - scale(12));
        const int groupWidth = clientWidth - margin * 2;
        const int inset = scale(16);
        const int advancedColumnGap = scale(24);
        const int columnWidth = (std::max)(
            scale(1),
            (groupWidth - inset * 2 - advancedColumnGap) / 2);
        const int left = margin + inset;
        const int right = left + columnWidth + advancedColumnGap;
        const int rowTop = panelTop + scale(32);
        const int nextRowTop = rowTop + scale(50);
        const int thirdRowTop = nextRowTop + scale(50);
        const int fourthRowTop = thirdRowTop + scale(50);

        switch (activeAdvancedSection_)
        {
        case AdvancedSection::Timing:
            moveControl(
                advancedTimingHeading_,
                margin,
                panelTop,
                groupWidth,
                groupHeight);
            layoutSlider(opacity_, left, rowTop, columnWidth, scale(40));
            layoutSlider(
                clickTimeScale_,
                left,
                nextRowTop,
                columnWidth,
                scale(40));
            layoutSlider(
                trailTimeScale_,
                right,
                rowTop,
                columnWidth,
                scale(40));
            layoutSlider(
                trailLifetimeMs_,
                right,
                nextRowTop,
                columnWidth,
                scale(40));
            break;
        case AdvancedSection::Particles:
            moveControl(
                advancedParticlesHeading_,
                margin,
                panelTop,
                groupWidth,
                groupHeight);
            layoutSlider(diskRadius_, left, rowTop, columnWidth, scale(40));
            layoutSlider(
                diskLifetimeMs_,
                left,
                nextRowTop,
                columnWidth,
                scale(40));
            layoutSlider(
                trailOpacity_,
                left,
                thirdRowTop,
                columnWidth,
                scale(40));
            layoutSlider(
                ringsHdrIntensity_,
                right,
                rowTop,
                columnWidth,
                scale(40));
            layoutSlider(
                shardsHdrIntensity_,
                right,
                nextRowTop,
                columnWidth,
                scale(40));
            moveControl(
                themeColorLabel_,
                left,
                fourthRowTop,
                scale(72),
                scale(40));
            moveControl(
                themeColorEdit_,
                left + scale(78),
                fourthRowTop,
                scale(112),
                scale(40));
            moveControl(
                themeColorPreview_,
                left + scale(196),
                fourthRowTop,
                scale(40),
                scale(40));
            moveControl(
                themeColorChoose_,
                left + scale(242),
                fourthRowTop,
                scale(78),
                scale(40));
            break;
        case AdvancedSection::Rings:
            moveControl(
                advancedRingsHeading_,
                margin,
                panelTop,
                groupWidth,
                groupHeight);
            layoutSlider(ringsCount_, left, rowTop, columnWidth, scale(40));
            layoutSlider(
                ringsLifetimeMs_,
                right,
                rowTop,
                columnWidth,
                scale(40));
            layoutSlider(
                ringsRadiusMin_,
                left,
                nextRowTop,
                columnWidth,
                scale(40));
            layoutSlider(
                ringsRadiusMax_,
                right,
                nextRowTop,
                columnWidth,
                scale(40));
            layoutSlider(
                ringsAngularVelocityMultiplier_,
                left,
                thirdRowTop,
                columnWidth,
                scale(40));
            layoutSlider(
                ringsRotationDirection_,
                right,
                thirdRowTop,
                columnWidth,
                scale(40));
            break;
        case AdvancedSection::ClickShards:
            moveControl(
                advancedClickShardsHeading_,
                margin,
                panelTop,
                groupWidth,
                groupHeight);
            layoutSlider(
                shardsClickCount_,
                left,
                rowTop,
                columnWidth,
                scale(40));
            layoutSlider(
                shardsClickRadius_,
                right,
                rowTop,
                columnWidth,
                scale(40));
            layoutSlider(
                shardsClickLifetimeMinMs_,
                left,
                nextRowTop,
                columnWidth,
                scale(40));
            layoutSlider(
                shardsClickLifetimeMaxMs_,
                right,
                nextRowTop,
                columnWidth,
                scale(40));
            layoutSlider(
                shardsClickSpeedMin_,
                left,
                thirdRowTop,
                columnWidth,
                scale(40));
            layoutSlider(
                shardsClickSpeedMax_,
                right,
                thirdRowTop,
                columnWidth,
                scale(40));
            layoutSlider(
                shardsSizeMin_,
                left,
                fourthRowTop,
                columnWidth,
                scale(40));
            layoutSlider(
                shardsSizeMax_,
                right,
                fourthRowTop,
                columnWidth,
                scale(40));
            break;
        case AdvancedSection::Bloom:
            moveControl(
                advancedBloomHeading_,
                margin,
                panelTop,
                groupWidth,
                groupHeight);
            layoutSlider(
                bloomDiffusion_,
                left,
                rowTop,
                columnWidth,
                scale(40));
            layoutSlider(
                bloomThreshold_,
                left,
                nextRowTop,
                columnWidth,
                scale(40));
            layoutSlider(
                bloomSoftKnee_,
                right,
                rowTop,
                columnWidth,
                scale(40));
            layoutSlider(
                bloomClamp_,
                right,
                nextRowTop,
                columnWidth,
                scale(40));
            break;
        case AdvancedSection::Layers:
        {
            moveControl(
                advancedLayersHeading_,
                margin,
                panelTop,
                groupWidth,
                groupHeight);
            const int layerColumnGap = scale(16);
            const int layerColumnWidth = (std::max)(
                scale(1),
                (groupWidth - inset * 2 - layerColumnGap * 2) / 3);
            const int layerMiddle = left + layerColumnWidth + layerColumnGap;
            const int layerRight = layerMiddle
                + layerColumnWidth + layerColumnGap;
            moveControl(
                diskLayerEnabled_,
                left,
                rowTop,
                layerColumnWidth,
                scale(40));
            moveControl(
                ringsLayerEnabled_,
                layerMiddle,
                rowTop,
                layerColumnWidth,
                scale(40));
            moveControl(
                clickShardsLayerEnabled_,
                layerRight,
                rowTop,
                layerColumnWidth,
                scale(40));
            moveControl(
                trailShardsLayerEnabled_,
                left,
                nextRowTop,
                layerColumnWidth,
                scale(40));
            moveControl(
                trailLayerEnabled_,
                layerMiddle,
                nextRowTop,
                layerColumnWidth,
                scale(40));
            moveControl(
                bloomLayerEnabled_,
                layerRight,
                nextRowTop,
                layerColumnWidth,
                scale(40));
            break;
        }
        }

        const int actionWidth = (clientWidth - margin * 2 - actionGap * 3) / 4;
        moveControl(
            pauseButton_,
            margin,
            actionY,
            actionWidth,
            actionHeight);
        moveControl(
            refreshButton_,
            margin + (actionWidth + actionGap),
            actionY,
            actionWidth,
            actionHeight);
        moveControl(
            hostLifecycleButton_,
            margin + (actionWidth + actionGap) * 2,
            actionY,
            actionWidth,
            actionHeight);
        moveControl(
            resetDefaultsButton_,
            margin + (actionWidth + actionGap) * 3,
            actionY,
            actionWidth,
            actionHeight);
        redrawWindowTree();
        return;
    }

    const int groupHeight = (std::max)(
        scale(350),
        clientHeight - contentTop - margin);
    moveControl(effectsHeading_, margin, contentTop, leftWidth, groupHeight);

    const int groupInset = scale(16);
    const int groupLeft = margin + groupInset;
    const int groupWidth = (std::max)(scale(1), leftWidth - groupInset * 2);
    moveControl(
        effectsModeLabel_,
        groupLeft,
        contentTop + scale(28),
        groupWidth,
        scale(22));
    moveControl(
        effectsMode_,
        groupLeft,
        contentTop + scale(50),
        groupWidth,
        scale(34));
    const int checkboxTop = contentTop + scale(88);
    const int checkboxWidth = groupWidth / 4;
    moveControl(effectsEnabled_, groupLeft, checkboxTop, checkboxWidth, scale(30));
    moveControl(clickEnabled_, groupLeft + checkboxWidth, checkboxTop, checkboxWidth, scale(30));
    moveControl(trailEnabled_, groupLeft + checkboxWidth * 2, checkboxTop, checkboxWidth, scale(30));
    moveControl(
        trailAlwaysOn_,
        groupLeft + checkboxWidth * 3,
        checkboxTop,
        groupWidth - checkboxWidth * 3,
        scale(30));

    // Keep the button-specific input switches on their own row so translated
    // labels remain readable on the smallest supported DPI-scaled layout.
    const int inputCheckboxTop = checkboxTop + scale(30);
    moveControl(
        leftClickEnabled_,
        groupLeft,
        inputCheckboxTop,
        checkboxWidth,
        scale(30));
    moveControl(
        rightClickEnabled_,
        groupLeft + checkboxWidth,
        inputCheckboxTop,
        checkboxWidth,
        scale(30));
    moveControl(
        middleClickEnabled_,
        groupLeft + checkboxWidth * 2,
        inputCheckboxTop,
        checkboxWidth,
        scale(30));

    int sliderTop = inputCheckboxTop + scale(30);
    layoutSlider(globalScale_, groupLeft, sliderTop, groupWidth, scale(40));
    sliderTop += scale(44);
    layoutSlider(trailLength_, groupLeft, sliderTop, groupWidth, scale(40));
    sliderTop += scale(44);
    layoutSlider(trailWidth_, groupLeft, sliderTop, groupWidth, scale(40));
    sliderTop += scale(44);
    layoutSlider(inputSamplingRate_, groupLeft, sliderTop, groupWidth, scale(40));
    sliderTop += scale(44);
    layoutSlider(bloomIntensity_, groupLeft, sliderTop, groupWidth, scale(40));
    sliderTop += scale(44);

    const int labelWidth = scale(106);
    moveControl(bloomQualityLabel_, groupLeft, sliderTop, labelWidth, scale(34));
    moveControl(
        bloomQuality_,
        groupLeft + labelWidth,
        sliderTop,
        groupWidth - labelWidth,
        scale(34));

    moveControl(backgroundHeading_, rightX, contentTop, availableRightWidth, groupHeight);
    const int rightContentX = rightX + groupInset;
    const int rightContentWidth = (std::max)(
        scale(1),
        availableRightWidth - groupInset * 2);
    moveControl(
        backgroundModeLabel_,
        rightContentX,
        contentTop + scale(32),
        rightContentWidth,
        scale(24));
    moveControl(
        backgroundMode_,
        rightContentX,
        contentTop + scale(58),
        rightContentWidth,
        scale(34));
    moveControl(
        cursorExcluded_,
        rightContentX,
        contentTop + scale(101),
        rightContentWidth,
        scale(30));
    moveControl(
        allowSystemBorder_,
        rightContentX,
        contentTop + scale(133),
        rightContentWidth,
        scale(30));
    moveControl(
        idleOptimization_,
        rightContentX,
        contentTop + scale(165),
        rightContentWidth,
        scale(30));
    moveControl(
        pauseButton_,
        rightContentX,
        contentTop + scale(201),
        rightContentWidth,
        scale(38));

    const int actionGap = scale(10);
    const int actionWidth = (rightContentWidth - actionGap) / 2;
    moveControl(
        refreshButton_,
        rightContentX,
        contentTop + scale(249),
        actionWidth,
        scale(38));
    moveControl(
        hostLifecycleButton_,
        rightContentX + actionWidth + actionGap,
        contentTop + scale(249),
        actionWidth,
        scale(38));
    moveControl(
        resetDefaultsButton_,
        rightContentX,
        contentTop + scale(297),
        rightContentWidth,
        scale(38));

    const int profileLabelWidth = scale(92);
    moveControl(
        fxProfileLabel_,
        rightContentX,
        contentTop + scale(341),
        profileLabelWidth,
        scale(34));
    moveControl(
        fxProfileSelector_,
        rightContentX + profileLabelWidth,
        contentTop + scale(341),
        rightContentWidth - profileLabelWidth,
        scale(34));

    const int profileButtonGap = scale(6);
    const int profileApplyWidth = scale(52);
    const int profileSaveWidth = scale(60);
    const int profileDeleteWidth = scale(58);
    const int profileNameWidth = (std::max)(
        scale(1),
        rightContentWidth
            - profileApplyWidth
            - profileSaveWidth
            - profileDeleteWidth
            - profileButtonGap * 3);
    const int profileRowTop = contentTop + scale(379);
    moveControl(
        fxProfileNameEdit_,
        rightContentX,
        profileRowTop,
        profileNameWidth,
        scale(34));
    const int profileApplyX = rightContentX
        + profileNameWidth
        + profileButtonGap;
    moveControl(
        applyFxProfileButton_,
        profileApplyX,
        profileRowTop,
        profileApplyWidth,
        scale(34));
    const int profileSaveX = profileApplyX
        + profileApplyWidth
        + profileButtonGap;
    moveControl(
        saveFxProfileButton_,
        profileSaveX,
        profileRowTop,
        profileSaveWidth,
        scale(34));
    moveControl(
        deleteFxProfileButton_,
        profileSaveX + profileSaveWidth + profileButtonGap,
        profileRowTop,
        profileDeleteWidth,
        scale(34));

    redrawWindowTree();
}

void ControlCenterWindow::redrawWindowTree() const noexcept
{
    if (window_ == nullptr)
    {
        return;
    }

    static_cast<void>(RedrawWindow(
        window_,
        nullptr,
        nullptr,
        RDW_INVALIDATE | RDW_ERASE | RDW_ALLCHILDREN | RDW_UPDATENOW));
}

void ControlCenterWindow::layoutSlider(
    const SliderControl& slider,
    const int x,
    const int y,
    const int width,
    const int height) const noexcept
{
    // English ring/shard labels need a little more room at the minimum width.
    const int labelWidth = scale(152);
    const int valueWidth = scale(64);
    const int gap = scale(8);
    moveControl(slider.label, x, y, labelWidth, height);
    moveControl(
        slider.trackbar,
        x + labelWidth,
        y,
        width - labelWidth - valueWidth - gap,
        height);
    moveControl(
        slider.valueText,
        x + width - valueWidth,
        y,
        valueWidth,
        height);
}

void ControlCenterWindow::setPageControlVisible(
    const HWND control,
    const bool visible) const noexcept
{
    if (control != nullptr)
    {
        ShowWindow(control, visible ? SW_SHOW : SW_HIDE);
    }
}

void ControlCenterWindow::updatePageVisibility() noexcept
{
    const bool hotkeys = activePage_ == Page::Hotkeys;
    setChecked(hotkeysPageButton_, hotkeys);
    for (const HWND control : hotkeyControls_)
    {
        setPageControlVisible(control, hotkeys);
    }
    const bool basic = activePage_ == Page::Basic;
    const bool advanced = activePage_ == Page::Advanced;
    const bool display = activePage_ == Page::DisplayPerformance;
    const bool system = activePage_ == Page::System;
    static_cast<void>(SendMessageW(
        basicPageButton_,
        BM_SETCHECK,
        basic ? BST_CHECKED : BST_UNCHECKED,
        0));
    static_cast<void>(SendMessageW(
        advancedPageButton_,
        BM_SETCHECK,
        advanced ? BST_CHECKED : BST_UNCHECKED,
        0));
    static_cast<void>(SendMessageW(
        displayPageButton_,
        BM_SETCHECK,
        display ? BST_CHECKED : BST_UNCHECKED,
        0));
    static_cast<void>(SendMessageW(
        systemPageButton_,
        BM_SETCHECK,
        system ? BST_CHECKED : BST_UNCHECKED,
        0));
    static_cast<void>(SendMessageW(
        advancedTimingSectionButton_,
        BM_SETCHECK,
        activeAdvancedSection_ == AdvancedSection::Timing
            ? BST_CHECKED
            : BST_UNCHECKED,
        0));
    static_cast<void>(SendMessageW(
        advancedParticlesSectionButton_,
        BM_SETCHECK,
        activeAdvancedSection_ == AdvancedSection::Particles
            ? BST_CHECKED
            : BST_UNCHECKED,
        0));
    static_cast<void>(SendMessageW(
        advancedRingsSectionButton_,
        BM_SETCHECK,
        activeAdvancedSection_ == AdvancedSection::Rings
            ? BST_CHECKED
            : BST_UNCHECKED,
        0));
    static_cast<void>(SendMessageW(
        advancedClickShardsSectionButton_,
        BM_SETCHECK,
        activeAdvancedSection_ == AdvancedSection::ClickShards
            ? BST_CHECKED
            : BST_UNCHECKED,
        0));
    static_cast<void>(SendMessageW(
        advancedBloomSectionButton_,
        BM_SETCHECK,
        activeAdvancedSection_ == AdvancedSection::Bloom
            ? BST_CHECKED
            : BST_UNCHECKED,
        0));
    static_cast<void>(SendMessageW(
        advancedLayersSectionButton_,
        BM_SETCHECK,
        activeAdvancedSection_ == AdvancedSection::Layers
            ? BST_CHECKED
            : BST_UNCHECKED,
        0));

    const std::array basicControls{
        effectsHeading_,
        effectsEnabled_,
        effectsModeLabel_,
        effectsMode_,
        clickEnabled_,
        trailEnabled_,
        trailAlwaysOn_,
        leftClickEnabled_,
        rightClickEnabled_,
        middleClickEnabled_,
        globalScale_.label,
        globalScale_.trackbar,
        globalScale_.valueText,
        trailLength_.label,
        trailLength_.trackbar,
        trailLength_.valueText,
        trailWidth_.label,
        trailWidth_.trackbar,
        trailWidth_.valueText,
        inputSamplingRate_.label,
        inputSamplingRate_.trackbar,
        inputSamplingRate_.valueText,
        bloomIntensity_.label,
        bloomIntensity_.trackbar,
        bloomIntensity_.valueText,
        bloomQualityLabel_,
        bloomQuality_,
        backgroundHeading_,
        backgroundModeLabel_,
        backgroundMode_,
        cursorExcluded_,
        allowSystemBorder_,
        idleOptimization_,
        fxProfileLabel_,
        fxProfileSelector_,
        fxProfileNameEdit_,
        applyFxProfileButton_,
        saveFxProfileButton_,
        deleteFxProfileButton_};
    for (const HWND control : basicControls)
    {
        setPageControlVisible(control, basic);
    }

    const std::array advancedSectionButtons{
        advancedTimingSectionButton_,
        advancedParticlesSectionButton_,
        advancedRingsSectionButton_,
        advancedClickShardsSectionButton_,
        advancedBloomSectionButton_,
        advancedLayersSectionButton_};
    for (const HWND control : advancedSectionButtons)
    {
        setPageControlVisible(control, advanced);
    }

    const bool timing = advanced
        && activeAdvancedSection_ == AdvancedSection::Timing;
    const std::array advancedTimingControls{
        advancedTimingHeading_,
        opacity_.label,
        opacity_.trackbar,
        opacity_.valueText,
        clickTimeScale_.label,
        clickTimeScale_.trackbar,
        clickTimeScale_.valueText,
        trailTimeScale_.label,
        trailTimeScale_.trackbar,
        trailTimeScale_.valueText,
        trailLifetimeMs_.label,
        trailLifetimeMs_.trackbar,
        trailLifetimeMs_.valueText};
    for (const HWND control : advancedTimingControls)
    {
        setPageControlVisible(control, timing);
    }

    const bool particles = advanced
        && activeAdvancedSection_ == AdvancedSection::Particles;
    const std::array advancedParticleControls{
        advancedParticlesHeading_,
        diskRadius_.label,
        diskRadius_.trackbar,
        diskRadius_.valueText,
        diskLifetimeMs_.label,
        diskLifetimeMs_.trackbar,
        diskLifetimeMs_.valueText,
        ringsHdrIntensity_.label,
        ringsHdrIntensity_.trackbar,
        ringsHdrIntensity_.valueText,
        shardsHdrIntensity_.label,
        shardsHdrIntensity_.trackbar,
        shardsHdrIntensity_.valueText,
        trailOpacity_.label,
        trailOpacity_.trackbar,
        trailOpacity_.valueText,
        themeColorLabel_,
        themeColorEdit_,
        themeColorPreview_,
        themeColorChoose_};
    for (const HWND control : advancedParticleControls)
    {
        setPageControlVisible(control, particles);
    }

    const bool rings = advanced
        && activeAdvancedSection_ == AdvancedSection::Rings;
    const std::array advancedRingControls{
        advancedRingsHeading_,
        ringsCount_.label,
        ringsCount_.trackbar,
        ringsCount_.valueText,
        ringsLifetimeMs_.label,
        ringsLifetimeMs_.trackbar,
        ringsLifetimeMs_.valueText,
        ringsRadiusMin_.label,
        ringsRadiusMin_.trackbar,
        ringsRadiusMin_.valueText,
        ringsRadiusMax_.label,
        ringsRadiusMax_.trackbar,
        ringsRadiusMax_.valueText,
        ringsAngularVelocityMultiplier_.label,
        ringsAngularVelocityMultiplier_.trackbar,
        ringsAngularVelocityMultiplier_.valueText,
        ringsRotationDirection_.label,
        ringsRotationDirection_.trackbar,
        ringsRotationDirection_.valueText};
    for (const HWND control : advancedRingControls)
    {
        setPageControlVisible(control, rings);
    }

    const bool clickShards = advanced
        && activeAdvancedSection_ == AdvancedSection::ClickShards;
    const std::array advancedClickShardControls{
        advancedClickShardsHeading_,
        shardsClickCount_.label,
        shardsClickCount_.trackbar,
        shardsClickCount_.valueText,
        shardsClickLifetimeMinMs_.label,
        shardsClickLifetimeMinMs_.trackbar,
        shardsClickLifetimeMinMs_.valueText,
        shardsClickLifetimeMaxMs_.label,
        shardsClickLifetimeMaxMs_.trackbar,
        shardsClickLifetimeMaxMs_.valueText,
        shardsClickRadius_.label,
        shardsClickRadius_.trackbar,
        shardsClickRadius_.valueText,
        shardsClickSpeedMin_.label,
        shardsClickSpeedMin_.trackbar,
        shardsClickSpeedMin_.valueText,
        shardsClickSpeedMax_.label,
        shardsClickSpeedMax_.trackbar,
        shardsClickSpeedMax_.valueText,
        shardsSizeMin_.label,
        shardsSizeMin_.trackbar,
        shardsSizeMin_.valueText,
        shardsSizeMax_.label,
        shardsSizeMax_.trackbar,
        shardsSizeMax_.valueText};
    for (const HWND control : advancedClickShardControls)
    {
        setPageControlVisible(control, clickShards);
    }

    const bool bloom = advanced
        && activeAdvancedSection_ == AdvancedSection::Bloom;
    const std::array advancedBloomControls{
        advancedBloomHeading_,
        bloomDiffusion_.label,
        bloomDiffusion_.trackbar,
        bloomDiffusion_.valueText,
        bloomThreshold_.label,
        bloomThreshold_.trackbar,
        bloomThreshold_.valueText,
        bloomSoftKnee_.label,
        bloomSoftKnee_.trackbar,
        bloomSoftKnee_.valueText,
        bloomClamp_.label,
        bloomClamp_.trackbar,
        bloomClamp_.valueText};
    for (const HWND control : advancedBloomControls)
    {
        setPageControlVisible(control, bloom);
    }

    const bool layers = advanced
        && activeAdvancedSection_ == AdvancedSection::Layers;
    const std::array advancedLayerControls{
        advancedLayersHeading_,
        diskLayerEnabled_,
        ringsLayerEnabled_,
        clickShardsLayerEnabled_,
        trailShardsLayerEnabled_,
        trailLayerEnabled_,
        bloomLayerEnabled_};
    for (const HWND control : advancedLayerControls)
    {
        setPageControlVisible(control, layers);
    }

    const std::array displayControls{
        displaySettingsHeading_,
        displaySelectorLabel_,
        displaySelector_,
        displaySummaryText_,
        hdrEnabled_,
        activeFxRoiEnabled_,
        framePacingLabel_,
        framePacing_,
        displayIndependent_,
        displayEffectsEnabled_,
        displayHdrEnabled_,
        displayFramePacingLabel_,
        displayFramePacing_,
        displayDetailsHeading_,
        displayDetailsText_,
        activeFxRoiDetailsHeading_,
        activeFxRoiDetailsText_};
    for (const HWND control : displayControls)
    {
        setPageControlVisible(control, display);
    }

    const std::array systemControls{
        systemSettingsHeading_,
        languageLabel_,
        languageSelector_,
        startWithWindows_,
        startMinimized_,
        closeToTray_,
        versionUpdateHeading_,
        controlCenterVersionText_,
        hostVersionText_,
        installStateText_,
        latestVersionText_,
        checkForUpdatesButton_,
        openReleaseButton_,
        repositoryStarHint_,
        openRepositoryButton_,
#if defined(BAFX_ENABLE_SPOUT2)
        spout2Enabled_,
        spout2SenderStatus_,
        obsSpoutPluginStatus_,
        spout2ObsHint_,
        refreshObsSpoutPluginButton_,
        openObsSpoutPluginPageButton_,
#endif
        openLogDirectoryButton_,
        clearLogsButton_};
    for (const HWND control : systemControls)
    {
        setPageControlVisible(control, system);
    }

    if (window_ != nullptr)
    {
        RECT client{};
        if (GetClientRect(window_, &client) != FALSE)
        {
            layoutControls(
                client.right - client.left,
                client.bottom - client.top);
            return;
        }
        redrawWindowTree();
    }
}

int ControlCenterWindow::scale(const int logicalPixels) const noexcept
{
    return MulDiv(logicalPixels, static_cast<int>(layoutDpi_), 96);
}

}
