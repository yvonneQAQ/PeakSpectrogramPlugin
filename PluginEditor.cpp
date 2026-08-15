#include "PluginProcessor.h"
#include "PluginEditor.h"
#include <BinaryData.h>


#include <algorithm>
#include <cmath> // 为了 std::log2, std::round
#include <cstring>
#include <limits>
#include <utility>


namespace
{
    constexpr int kMaxManualFrozenChords = 100;
    constexpr int kVisibleFrozenChords = 20;
    constexpr int kMaxAutoChordsSafety = 128;
    constexpr int kMaxDetailAnalysisFrames = 2400;
    constexpr double kMaxAutoDurationSeconds = 30.0;
    constexpr double kAutoMinimumGapMs = 100.0;
    constexpr float kPeakIntensityFloorDbFs = -72.0f;
    constexpr float kPeakIntensityCeilingDbFs = -6.0f;
    constexpr int kMinimumPartialVelocity = 1;
    constexpr float kButtonBackgroundAlpha = 0.75f;
    constexpr float kModuleScale = 0.85f;
    constexpr int kLeftColumnShift = 24;
    constexpr int kRightColumnShift = 0;
    constexpr int kEditorWidth = 1032;
    constexpr int kEditorHeight = 624;
    constexpr int kOuterMargin = 12;
    constexpr int kRightPanelWidth = 222;
    constexpr int kMainColumnGap = 9;
    constexpr int kSpectrogramTopOffset = 37;
    constexpr int kSpectrogramBottomGap = 9;
    constexpr int kTitleAreaHeight = 44;
    constexpr int kRightPanelInset = 12;
    constexpr int kBottomInfoHeight = 25;
    constexpr int kPeakTextAreaHeight = 132;
    constexpr int kDetailHeaderHeight = 25;
    constexpr int kDetailDragWidth = 155;
    constexpr int kDetailTimeScaleWidth = 185;
    constexpr float kUnifiedCornerRadius = 12.0f;

    using PartialIntensityArray =
        std::array<float, AudioPluginAudioProcessor::kNumNoisyPeaks>;

    PartialIntensityArray calculatePartialIntensities (
        const std::array<float, AudioPluginAudioProcessor::kNumNoisyPeaks>& freqsHz,
        const std::array<float, AudioPluginAudioProcessor::kNumNoisyPeaks>& peakLevelsDbFs,
        int activePeakCount)
    {
        PartialIntensityArray intensities {};
        const int peakCount = juce::jlimit (0,
                                            AudioPluginAudioProcessor::kNumNoisyPeaks,
                                            activePeakCount);

        for (int peakIndex = 0; peakIndex < peakCount; ++peakIndex)
        {
            if (freqsHz[(size_t) peakIndex] <= 0.0f)
                continue;

            const float levelDbFs = peakLevelsDbFs[(size_t) peakIndex];
            if (! std::isfinite (levelDbFs))
                continue;

            const float clampedDbFs = juce::jlimit (kPeakIntensityFloorDbFs,
                                                     kPeakIntensityCeilingDbFs,
                                                     levelDbFs);
            intensities[(size_t) peakIndex] = juce::jmap (clampedDbFs,
                                                           kPeakIntensityFloorDbFs,
                                                           kPeakIntensityCeilingDbFs,
                                                           0.0f,
                                                           1.0f);
        }

        return intensities;
    }

    juce::Path makeOrganicCapsulePath (juce::Rectangle<float> bounds, float expression)
    {
        expression = juce::jlimit (0.0f, 1.0f, expression);

        const float x = bounds.getX();
        const float y = bounds.getY();
        const float w = bounds.getWidth();
        const float h = bounds.getHeight();
        const auto px = [x, w] (float proportion) { return x + w * proportion; };
        const auto py = [y, h] (float proportion) { return y + h * proportion; };

        const float topLeft = 0.075f + 0.018f * expression;
        const float topMiddle = 0.050f - 0.024f * expression;
        const float topRight = 0.080f + 0.035f * expression;
        const float rightMiddle = 0.988f + 0.008f * expression;
        const float bottomRight = 0.915f - 0.018f * expression;
        const float bottomMiddle = 0.950f + 0.030f * expression;
        const float bottomLeft = 0.920f + 0.020f * expression;
        const float leftMiddle = 0.012f - 0.006f * expression;

        juce::Path path;
        path.startNewSubPath (px (leftMiddle), py (0.50f + 0.018f * expression));
        path.cubicTo (px (-0.005f), py (0.25f),
                      px (0.065f), py (topLeft),
                      px (0.215f), py (topLeft));
        path.cubicTo (px (0.390f), py (topMiddle),
                      px (0.610f), py (0.070f + 0.018f * expression),
                      px (0.790f), py (topRight));
        path.cubicTo (px (0.935f), py (topRight + 0.005f),
                      px (rightMiddle), py (0.285f),
                      px (rightMiddle), py (0.490f - 0.012f * expression));
        path.cubicTo (px (1.000f), py (0.740f),
                      px (0.915f), py (bottomRight),
                      px (0.755f), py (bottomRight));
        path.cubicTo (px (0.565f), py (bottomMiddle),
                      px (0.365f), py (0.900f + 0.020f * expression),
                      px (0.205f), py (bottomLeft));
        path.cubicTo (px (0.065f), py (bottomLeft),
                      px (leftMiddle), py (0.760f),
                      px (leftMiddle), py (0.50f + 0.018f * expression));
        path.closeSubPath();
        return path;
    }

    // Photograph theme: translucent neutral panels preserve the image while
    // white typography and a warm spectral accent maintain legibility.
    const juce::Colour kEditorBackground = juce::Colour::fromRGB (39, 33, 33);     // #272121 fallback
    const juce::Colour kPanelBackground = juce::Colour::fromRGB (225, 225, 225);
    const juce::Colour kPanelTint = juce::Colour::fromRGB (238, 238, 238);
    const juce::Colour kTextPrimary = juce::Colour::fromRGB (23, 19, 19);          // #171313
    const juce::Colour kTextSecondary = juce::Colour::fromRGB (74, 70, 68);        // deep grey
    const juce::Colour kButtonTerracotta = juce::Colour::fromRGB (210, 210, 210);
    const juce::Colour kButtonTerracottaStrong = juce::Colour::fromRGB (78, 78, 78);
    const juce::Colour kButtonTerracottaMuted = juce::Colour::fromRGB (227, 137, 41); // #E38929
    const juce::Colour kAmberText = juce::Colour::fromRGB (245, 192, 91); // #F5C05B
    const juce::Colour kContrastBlush = kTextPrimary;
    const juce::Colour kButtonText = kTextPrimary;
    const juce::Colour kLargePanelColour = juce::Colour::fromRGB (238, 233, 223).withAlpha (0.42f);
    const juce::Colour kPanelUnderlayColour = juce::Colour::fromRGB (235, 230, 220).withAlpha (0.14f);
    const juce::Colour kSidebarPanelColour = juce::Colour::fromRGB (242, 238, 230).withAlpha (0.46f);
    const juce::Colour kSidebarUnderlayColour = juce::Colour::fromRGB (236, 231, 220).withAlpha (0.16f);
    const juce::Colour kBackgroundVeil = juce::Colour::fromRGB (210, 205, 190).withAlpha (0.10f);
    const juce::Colour kSpectrogramCanvasBackground = juce::Colour::fromRGB (217, 216, 203).withAlpha (0.96f);
    const juce::Colour kSpectrogramLine = juce::Colour::fromRGB (92, 94, 86).withAlpha (0.11f);
    const juce::Colour kSpectrogramStaticLabel = juce::Colour::fromRGB (91, 89, 84).withAlpha (0.78f);
    const juce::Colour kSpectrogramLabel = kButtonTerracottaMuted;
    const juce::Colour kFreezeMarker = juce::Colour::fromRGB (137, 166, 124);      // #89A67C
    const juce::Colour kDetailDivider = juce::Colours::black.withAlpha (0.10f);

    juce::Colour spectrogramColourForLevel (float norm)
    {
        norm = juce::jlimit (0.0f, 1.0f, norm);
        norm = std::pow (norm, 1.35f);

        // Keep quiet bins close to the canvas, then travel through several
        // distinct hues before reaching orange. This makes small level
        // differences readable instead of turning a broadband frame into one
        // nearly uniform warm block.
        const auto quiet = kSpectrogramCanvasBackground.withAlpha (1.0f);
        const auto low = juce::Colour::fromRGB (174, 184, 164);   // muted sage
        const auto mid = juce::Colour::fromRGB (204, 193, 151);   // olive sand
        const auto high = juce::Colour::fromRGB (230, 168, 64);   // amber
        const auto peak = juce::Colour::fromRGB (218, 91, 43);    // terracotta

        if (norm < 0.18f)
            return quiet.interpolatedWith (low, norm / 0.18f);

        if (norm < 0.36f)
            return low.interpolatedWith (mid, (norm - 0.18f) / 0.18f);

        if (norm < 0.76f)
            return mid.interpolatedWith (high, (norm - 0.36f) / 0.40f);

        return high.interpolatedWith (peak, (norm - 0.76f) / 0.24f);
    }

    const juce::Image& getEditorBackgroundImage()
    {
        static const auto image = []
        {
            return juce::ImageFileFormat::loadFrom (
                SPEKANAAssets::forest_background_jpeg,
                SPEKANAAssets::forest_background_jpegSize);
        }();
        return image;
    }

    void drawImageCover (juce::Graphics& g, const juce::Image& image, juce::Rectangle<float> destination)
    {
        if (! image.isValid() || destination.isEmpty())
            return;

        auto source = image.getBounds().toFloat();
        const float destinationAspect = destination.getWidth() / destination.getHeight();
        const float sourceAspect = source.getWidth() / source.getHeight();

        if (sourceAspect > destinationAspect)
        {
            const float croppedWidth = source.getHeight() * destinationAspect;
            source = source.withSizeKeepingCentre (croppedWidth, source.getHeight());
        }
        else
        {
            const float croppedHeight = source.getWidth() / destinationAspect;
            source = source.withSizeKeepingCentre (source.getWidth(), croppedHeight);
        }

        const auto destinationPixels = destination.toNearestInt();
        const auto sourcePixels = source.toNearestInt();
        g.drawImage (image,
                     destinationPixels.getX(), destinationPixels.getY(),
                     destinationPixels.getWidth(), destinationPixels.getHeight(),
                     sourcePixels.getX(), sourcePixels.getY(),
                     sourcePixels.getWidth(), sourcePixels.getHeight());
    }

    juce::Rectangle<int> shrinkScoreArea (juce::Rectangle<int> area)
    {
        return area.withSizeKeepingCentre (area.getWidth(),
                                           (int) std::round ((float) area.getHeight() * 0.90f));
    }

    juce::Rectangle<int> getModuleBounds (juce::Rectangle<int> windowBounds)
    {
        const auto available = windowBounds.reduced (kOuterMargin);
        return available.withSizeKeepingCentre ((int) std::round ((float) available.getWidth() * kModuleScale),
                                                (int) std::round ((float) available.getHeight() * kModuleScale));
    }

    int getExpandedSpectrogramHeight (int leftPanelHeight)
    {
        const auto previousAvailableHeight = juce::jmax (0, leftPanelHeight - kSpectrogramTopOffset);
        return (int) std::round ((float) previousAvailableHeight * 0.39f * 1.20f);
    }

    juce::String musicGlyph (juce::juce_wchar codepoint)
    {
        return juce::String::charToString (codepoint);
    }

    juce::Typeface::Ptr getMusicTypeface()
    {
        static juce::Typeface::Ptr cachedTypeface;
        static bool attemptedLoad = false;

        if (attemptedLoad)
            return cachedTypeface;

        attemptedLoad = true;

        const auto executableFile = juce::File::getSpecialLocation (juce::File::currentExecutableFile);
        const auto executableDir = executableFile.getParentDirectory();
        const auto workingDir = juce::File::getCurrentWorkingDirectory();
        const juce::File candidateFiles[] =
        {
            executableDir.getChildFile ("Assets").getChildFile ("Bravura.otf"),
            executableDir.getChildFile ("Assets").getChildFile ("BravuraText.otf"),
            executableDir.getSiblingFile ("Resources").getChildFile ("Assets").getChildFile ("Bravura.otf"),
            executableDir.getSiblingFile ("Resources").getChildFile ("Assets").getChildFile ("BravuraText.otf"),
            workingDir.getChildFile ("Assets").getChildFile ("Bravura.otf"),
            workingDir.getChildFile ("Assets").getChildFile ("BravuraText.otf"),
            juce::File ("/System/Library/Fonts/Apple Symbols.ttf"),
            juce::File ("/System/Library/Fonts/Supplemental/Arial Unicode.ttf"),
            juce::File ("/Library/Fonts/Arial Unicode.ttf")
        };

        for (const auto& file : candidateFiles)
        {
            if (! file.existsAsFile())
                continue;

            juce::MemoryBlock fontData;
            if (! file.loadFileAsData (fontData))
                continue;

            cachedTypeface = juce::Typeface::createSystemTypefaceFor (fontData.getData(), fontData.getSize());
            if (cachedTypeface != nullptr)
                return cachedTypeface;
        }

        return {};
    }

    juce::Font makeMusicFont (float height)
    {
        auto options = juce::FontOptions {}
                           .withHeight (height * 1.20f)
                           .withFallbackEnabled (true)
                           .withFallbacks ({ "Apple Symbols", "Arial Unicode MS", "Arial Unicode" });

        if (auto typeface = getMusicTypeface())
            options = options.withTypeface (typeface);

        return juce::Font (options);
    }

    juce::Font makeUIFont (float height, bool bold = false)
    {
        constexpr float uiFontScale = 1.44f;
        auto options = juce::FontOptions {}
                           .withName ("Avenir Next")
                           .withHeight (height * uiFontScale)
                           .withFallbackEnabled (true)
                           .withFallbacks ({ "Avenir", "PingFang SC", "Hiragino Sans GB", "Helvetica Neue" });

        if (bold)
            options = options.withStyle ("Demi Bold");

        return juce::Font (options);
    }

    juce::Font makeTitleFont (float height)
    {
        auto options = juce::FontOptions {}
                           .withName ("Avenir Next")
                           .withHeight (height * 1.20f)
                           .withFallbackEnabled (true)
                           .withFallbacks ({ "Avenir", "Helvetica Neue" })
                           .withStyle ("Regular");

        return juce::Font (options);
    }

    void drawTrackedText (juce::Graphics& g,
                          const juce::String& text,
                          juce::Rectangle<float> bounds,
                          const juce::Font& font,
                          juce::Colour colour,
                          float tracking)
    {
        juce::GlyphArrangement glyphs;
        glyphs.addLineOfText (font, text, 0.0f, font.getAscent());
        for (int index = 1; index < glyphs.getNumGlyphs(); ++index)
            glyphs.getGlyph (index).moveBy (tracking * (float) index, 0.0f);

        glyphs.justifyGlyphs (0,
                              glyphs.getNumGlyphs(),
                              bounds.getX(),
                              bounds.getY(),
                              bounds.getWidth(),
                              bounds.getHeight(),
                              juce::Justification::centred);
        g.setColour (colour);
        glyphs.draw (g);
    }

    juce::String accidentalGlyphForStaff (const juce::String& accidental)
    {
        if (accidental == "#")
            return juce::String (juce::CharPointer_UTF8 ("\xE2\x99\xAF"));

        if (accidental == "b")
            return juce::String (juce::CharPointer_UTF8 ("\xE2\x99\xAD"));

        if (accidental == "q#")
            return juce::String (juce::CharPointer_UTF8 ("\xE2\x86\x91"));

        if (accidental == "qb")
            return juce::String (juce::CharPointer_UTF8 ("\xE2\x86\x93"));

        return {};
    }

    void drawQuarterToneArrowMark (juce::Graphics& g, juce::Rectangle<float> bounds, bool pointsUp)
    {
        const float centreX = bounds.getCentreX();
        const float shaftTop = bounds.getY() + (pointsUp ? 2.0f : 1.0f);
        const float shaftBottom = bounds.getBottom() - (pointsUp ? 1.0f : 2.0f);

        g.drawLine (centreX, shaftTop, centreX, shaftBottom, 1.1f);

        juce::Path arrowHead;
        if (pointsUp)
        {
            const float tipY = bounds.getY();
            arrowHead.startNewSubPath (centreX, tipY);
            arrowHead.lineTo (centreX - 2.4f, tipY + 3.2f);
            arrowHead.lineTo (centreX + 2.4f, tipY + 3.2f);
        }
        else
        {
            const float tipY = bounds.getBottom();
            arrowHead.startNewSubPath (centreX, tipY);
            arrowHead.lineTo (centreX - 2.4f, tipY - 3.2f);
            arrowHead.lineTo (centreX + 2.4f, tipY - 3.2f);
        }

        arrowHead.closeSubPath();
        g.fillPath (arrowHead);
    }

    class MinimalPluginLookAndFeel : public juce::LookAndFeel_V4
    {
    public:
        void preparePopupMenuWindow (juce::Component& window) override
        {
            window.setOpaque (false);
        }

        void drawPopupMenuBackground (juce::Graphics& g, int width, int height) override
        {
            g.setColour (juce::Colours::transparentBlack);
            g.fillAll();

            auto bounds = juce::Rectangle<float> (0.0f, 0.0f, (float) width, (float) height).reduced (1.0f);
            g.setColour (juce::Colour::fromRGB (164, 167, 161).withAlpha (0.30f));
            g.fillRoundedRectangle (bounds, kUnifiedCornerRadius);
            g.setColour (juce::Colours::white.withAlpha (0.18f));
            g.drawRoundedRectangle (bounds, kUnifiedCornerRadius, 0.8f);
        }

        void drawPopupMenuItem (juce::Graphics& g,
                                const juce::Rectangle<int>& area,
                                bool isSeparator,
                                bool isActive,
                                bool isHighlighted,
                                bool isTicked,
                                bool hasSubMenu,
                                const juce::String& text,
                                const juce::String& shortcutKeyText,
                                const juce::Drawable* icon,
                                const juce::Colour* textColour) override
        {
            juce::ignoreUnused (isTicked, hasSubMenu, shortcutKeyText, icon, textColour);

            if (isSeparator)
                return;

            auto item = area.reduced (5, 2);
            if (isHighlighted && isActive)
            {
                g.setColour (juce::Colour::fromRGB (119, 137, 102).withAlpha (0.28f));
                g.fillRoundedRectangle (item.toFloat(), 8.0f);
            }

            auto marker = item.removeFromLeft (13);
            g.setColour ((isHighlighted ? kButtonTerracottaMuted : kTextSecondary)
                             .withAlpha (isActive ? (isHighlighted ? 0.88f : 0.36f) : 0.16f));
            g.fillEllipse ((float) marker.getCentreX() - 2.2f,
                           (float) marker.getCentreY() - 2.2f,
                           4.4f,
                           4.4f);

            g.setColour (kTextPrimary.withAlpha (isActive ? 0.92f : 0.36f));
            g.setFont (makeUIFont (8.0f, false));
            g.drawText (text, item.reduced (2, 0), juce::Justification::centredLeft);
        }

        void getIdealPopupMenuItemSize (const juce::String& text,
                                        bool isSeparator,
                                        int standardMenuItemHeight,
                                        int& idealWidth,
                                        int& idealHeight) override
        {
            juce::ignoreUnused (text, standardMenuItemHeight);
            idealWidth = isSeparator ? 100 : 112;
            idealHeight = isSeparator ? 4 : 24;
        }

        juce::Font getTextButtonFont (juce::TextButton& button, int buttonHeight) override
        {
            const bool useLargeText = (bool) button.getProperties().getWithDefault (
                juce::Identifier ("largeButtonText"), false);
            const float fontSizeOffset = (float) (double) button.getProperties().getWithDefault (
                juce::Identifier ("fontSizeOffset"), 0.0);
            const float height = (float) buttonHeight * (useLargeText ? 0.56f : 0.43f)
                               + fontSizeOffset;

            return juce::Font (juce::FontOptions {}
                                   .withName ("Avenir Next")
                                   .withStyle (useLargeText ? "Regular" : "Demi Bold")
                                   .withHeight (height)
                                   .withFallbackEnabled (true)
                                   .withFallbacks ({ "Avenir", "Helvetica Neue" }));
        }

        void drawButtonBackground (juce::Graphics& g,
                                   juce::Button& button,
                                   const juce::Colour& backgroundColour,
                                   bool shouldDrawButtonAsHighlighted,
                                   bool shouldDrawButtonAsDown) override
        {
            auto bounds = button.getLocalBounds().toFloat().reduced (0.5f);
            const bool isTextTab = (bool) button.getProperties().getWithDefault (
                juce::Identifier ("textTab"), false);
            const bool isParameterToggle = (bool) button.getProperties().getWithDefault (
                juce::Identifier ("parameterToggle"), false);
            const bool isPrimaryAction = (bool) button.getProperties().getWithDefault (
                juce::Identifier ("primaryAction"), false);
            const bool isSecondaryAction = (bool) button.getProperties().getWithDefault (
                juce::Identifier ("secondaryAction"), false);
            const bool isOutlineAction = (bool) button.getProperties().getWithDefault (
                juce::Identifier ("outlineAction"), false);

            if (isTextTab)
            {
                if (button.getToggleState())
                {
                    g.setColour (kButtonTerracottaMuted);
                    g.fillRect (bounds.getX() + 3.0f,
                                bounds.getBottom() - 3.5f,
                                bounds.getWidth() - 6.0f,
                                3.0f);
                }
                return;
            }

            if (isParameterToggle)
                return;

            if (isPrimaryAction)
            {
                const bool organicActionIsActive = button.getToggleState();
                auto flowerLight = juce::Colour::fromRGB (255, 203, 62);  // sampled golden highlight
                auto flowerGold = juce::Colour::fromRGB (247, 177, 18);   // dominant reference colour
                auto flowerDeep = juce::Colour::fromRGB (218, 137, 5);    // shaded lower edge
                if (! button.isEnabled() && ! organicActionIsActive)
                {
                    flowerLight = flowerLight.withMultipliedAlpha (0.34f);
                    flowerGold = flowerGold.withMultipliedAlpha (0.34f);
                    flowerDeep = flowerDeep.withMultipliedAlpha (0.34f);
                }
                else if (shouldDrawButtonAsDown)
                {
                    flowerLight = flowerLight.darker (0.10f);
                    flowerGold = flowerGold.darker (0.10f);
                    flowerDeep = flowerDeep.darker (0.10f);
                }
                else if (shouldDrawButtonAsHighlighted)
                {
                    flowerLight = flowerLight.brighter (0.06f);
                    flowerGold = flowerGold.brighter (0.06f);
                }

                const auto flower = bounds.withSizeKeepingCentre (juce::jmin (84.0f, bounds.getWidth()),
                                                                  juce::jmin (34.0f, bounds.getHeight()));
                const auto blossom = makeOrganicCapsulePath (flower, 0.92f);

                if (organicActionIsActive)
                {
                    juce::ColourGradient flowerGradient (flowerLight.withAlpha (0.30f),
                                                          flower.getCentreX() - 8.0f,
                                                          flower.getY(),
                                                          flowerDeep.withAlpha (0.30f),
                                                          flower.getCentreX() + 5.0f,
                                                          flower.getBottom(),
                                                          false);
                    flowerGradient.addColour (0.48, flowerGold.withAlpha (0.30f));
                    g.setGradientFill (flowerGradient);
                    g.fillPath (blossom);

                    g.setColour (flowerGold.withAlpha (0.58f));
                    g.strokePath (blossom,
                                  juce::PathStrokeType (1.25f,
                                                        juce::PathStrokeType::curved,
                                                        juce::PathStrokeType::rounded));

                    g.setColour (juce::Colours::white.withAlpha (0.05f));
                    g.fillEllipse (flower.getCentreX() - 17.0f,
                                   flower.getCentreY() - 10.0f,
                                   31.0f,
                                   15.0f);
                }
                else
                {
                    g.setColour (flowerLight.withAlpha (button.isEnabled() ? 0.12f : 0.05f));
                    g.fillPath (blossom);
                    g.setColour (flowerGold.withAlpha (button.isEnabled() ? 0.72f : 0.28f));
                    g.strokePath (blossom,
                                  juce::PathStrokeType (1.35f,
                                                        juce::PathStrokeType::curved,
                                                        juce::PathStrokeType::rounded));
                }
                return;
            }

            if (isSecondaryAction || isOutlineAction)
            {
                auto colour = juce::Colours::white.withAlpha (isOutlineAction ? 0.10f : 0.18f);

                if (! button.isEnabled())
                    colour = colour.withMultipliedAlpha (0.38f);
                else if (shouldDrawButtonAsDown)
                    colour = colour.darker (0.10f);
                else if (shouldDrawButtonAsHighlighted)
                    colour = colour.brighter (0.06f);

                const auto organicBounds = bounds.reduced (0.2f);
                const auto organicPath = makeOrganicCapsulePath (organicBounds,
                                                                 isOutlineAction ? 0.22f : 0.28f);
                g.setColour (colour);
                g.fillPath (organicPath);

                g.setColour ((isOutlineAction ? juce::Colours::white : kTextPrimary)
                                 .withAlpha (isOutlineAction ? 0.16f : 0.04f));
                g.strokePath (organicPath,
                              juce::PathStrokeType (0.9f,
                                                    juce::PathStrokeType::curved,
                                                    juce::PathStrokeType::rounded));
                return;
            }

            auto colour = button.getToggleState()
                            ? button.findColour (juce::TextButton::buttonOnColourId)
                            : backgroundColour;

            if (! button.isEnabled())
                colour = kButtonTerracotta.darker (0.18f);
            else if (shouldDrawButtonAsDown)
                colour = colour.darker (0.12f);
            else if (shouldDrawButtonAsHighlighted)
                colour = colour.brighter (0.06f);

            const auto requestedOpacity = (float) (double) button.getProperties().getWithDefault (
                juce::Identifier ("backgroundOpacity"), (double) kButtonBackgroundAlpha);
            colour = colour.withAlpha (juce::jlimit (0.0f, 1.0f, requestedOpacity));

            auto shadowBounds = bounds.translated (0.0f, 1.3f);
            g.setColour (juce::Colours::black.withAlpha (button.isEnabled() ? 0.11f : 0.05f));
            g.fillRoundedRectangle (shadowBounds, kUnifiedCornerRadius);

            g.setColour (colour);
            g.fillRoundedRectangle (bounds, kUnifiedCornerRadius);

            g.setColour (juce::Colours::white.withAlpha (0.10f));
            g.drawRoundedRectangle (bounds.reduced (0.8f), kUnifiedCornerRadius, 0.9f);
        }

        void drawButtonText (juce::Graphics& g,
                             juce::TextButton& button,
                             bool,
                             bool) override
        {
            const bool isTextTab = (bool) button.getProperties().getWithDefault (
                juce::Identifier ("textTab"), false);
            const bool isParameterToggle = (bool) button.getProperties().getWithDefault (
                juce::Identifier ("parameterToggle"), false);
            const bool isOutlineAction = (bool) button.getProperties().getWithDefault (
                juce::Identifier ("outlineAction"), false);
            const bool isPrimaryAction = (bool) button.getProperties().getWithDefault (
                juce::Identifier ("primaryAction"), false);

            if (isTextTab || isParameterToggle)
            {
                const auto textColour = isParameterToggle
                                          ? (button.getToggleState() ? kAmberText : kTextPrimary)
                                          : (button.getToggleState() ? kTextPrimary : kTextSecondary);
                g.setColour (textColour.withAlpha (button.isEnabled() ? 0.98f : 0.38f));
                g.setFont (makeUIFont (12.5f, false));
                auto textBounds = button.getLocalBounds().reduced (isParameterToggle ? 4 : 6, 2);
                g.drawFittedText (button.getButtonText(),
                                  textBounds,
                                  isParameterToggle ? juce::Justification::centredLeft
                                                    : juce::Justification::centred,
                                  1);
                return;
            }

            if (isOutlineAction)
            {
                auto textBounds = button.getLocalBounds().reduced (9, 2);
                auto iconBounds = textBounds.removeFromLeft (14).toFloat().reduced (3.0f, 2.8f);
                auto arrowBounds = textBounds.removeFromRight (13).toFloat();
                juce::Path fileIcon;
                fileIcon.startNewSubPath (iconBounds.getX(), iconBounds.getY());
                fileIcon.lineTo (iconBounds.getRight() - 3.5f, iconBounds.getY());
                fileIcon.lineTo (iconBounds.getRight(), iconBounds.getY() + 3.5f);
                fileIcon.lineTo (iconBounds.getRight(), iconBounds.getBottom());
                fileIcon.lineTo (iconBounds.getX(), iconBounds.getBottom());
                fileIcon.closeSubPath();
                g.setColour (kButtonTerracottaMuted.withAlpha (button.isEnabled() ? 0.95f : 0.34f));
                g.strokePath (fileIcon, juce::PathStrokeType (1.2f));

                g.setColour (kTextPrimary.withAlpha (button.isEnabled() ? 0.98f : 0.38f));
                g.setFont (makeUIFont (10.0f, false));
                g.drawFittedText (button.getButtonText(),
                                  textBounds.reduced (2, 0),
                                  juce::Justification::centredLeft,
                                  1);

                const float arrowY = arrowBounds.getCentreY();
                const float arrowLeft = arrowBounds.getX() + 2.0f;
                const float arrowRight = arrowBounds.getRight() - 2.0f;
                g.setColour (kTextSecondary.withAlpha (button.isEnabled() ? 0.70f : 0.24f));
                g.drawLine (arrowLeft, arrowY, arrowRight, arrowY, 1.0f);
                g.drawLine (arrowRight - 3.0f, arrowY - 2.5f, arrowRight, arrowY, 1.0f);
                g.drawLine (arrowRight - 3.0f, arrowY + 2.5f, arrowRight, arrowY, 1.0f);
                return;
            }

            if (isPrimaryAction)
            {
                const bool active = button.getToggleState();
                g.setColour (kTextPrimary.withAlpha (active || button.isEnabled() ? 0.94f : 0.34f));
                g.setFont (makeUIFont (10.2f, false));
                g.drawFittedText (button.getButtonText(),
                                  button.getLocalBounds().withSizeKeepingCentre (62, 22),
                                  juce::Justification::centred,
                                  1);
                return;
            }

            g.setColour (button.findColour (button.getToggleState() ? juce::TextButton::textColourOnId
                                                                    : juce::TextButton::textColourOffId));
            g.setFont (getTextButtonFont (button, button.getHeight()));
            g.drawFittedText (button.getButtonText(),
                              button.getLocalBounds().reduced (8, 4),
                              juce::Justification::centred,
                              1);
        }
    };

    void styleTextButton (juce::TextButton& button, bool emphasised = false)
    {
        juce::ignoreUnused (emphasised);
        const auto offColour = kButtonTerracotta;
        const auto onColour = kButtonTerracottaStrong;

        button.setColour (juce::TextButton::buttonColourId, offColour);
        button.setColour (juce::TextButton::buttonOnColourId, onColour);
        button.setColour (juce::TextButton::textColourOffId, kTextPrimary);
        button.setColour (juce::TextButton::textColourOnId, kAmberText);
    }

    struct PitchNotation
    {
        bool valid = false;
        juce::String displayLabel;
        juce::String accidentalForStaff;
        int diatonicNumber = 0;
        float intensity = 1.0f;
    };

    struct QuantisedPitchClass
    {
        const char* baseName;
        int degree;
        const char* inlineAccidental;
        const char* staffAccidental;
    };

    constexpr QuantisedPitchClass kSemitonePitchClasses[12] =
    {
        { "C", 0, "", "" },
        { "C", 0, "#", "#" },
        { "D", 1, "", "" },
        { "D", 1, "#", "#" },
        { "E", 2, "", "" },
        { "F", 3, "", "" },
        { "F", 3, "#", "#" },
        { "G", 4, "", "" },
        { "G", 4, "#", "#" },
        { "A", 5, "", "" },
        { "A", 5, "#", "#" },
        { "B", 6, "", "" }
    };

    constexpr QuantisedPitchClass kQuarterTonePitchClasses[24] =
    {
        { "C", 0, "", "" },
        { "C", 0, "\xE2\x86\x91", "q#" },
        { "C", 0, "#", "#" },
        { "D", 1, "\xE2\x86\x93", "qb" },
        { "D", 1, "", "" },
        { "D", 1, "\xE2\x86\x91", "q#" },
        { "D", 1, "#", "#" },
        { "E", 2, "\xE2\x86\x93", "qb" },
        { "E", 2, "", "" },
        { "E", 2, "\xE2\x86\x91", "q#" },
        { "F", 3, "", "" },
        { "F", 3, "\xE2\x86\x91", "q#" },
        { "F", 3, "#", "#" },
        { "G", 4, "\xE2\x86\x93", "qb" },
        { "G", 4, "", "" },
        { "G", 4, "\xE2\x86\x91", "q#" },
        { "G", 4, "#", "#" },
        { "A", 5, "\xE2\x86\x93", "qb" },
        { "A", 5, "", "" },
        { "A", 5, "\xE2\x86\x91", "q#" },
        { "A", 5, "#", "#" },
        { "B", 6, "\xE2\x86\x93", "qb" },
        { "B", 6, "", "" },
        { "B", 6, "\xE2\x86\x91", "q#" }
    };

    PitchNotation quantiseFrequencyToPitchNotation (float freqHz, bool useQuarterToneMode)
    {
        if (freqHz <= 0.0f)
            return {};

        if (useQuarterToneMode)
        {
            const float quarterToneIndexFloat = 138.0f + 24.0f * std::log2 (freqHz / 440.0f);
            const int quarterToneIndex = (int) std::round (quarterToneIndexFloat);

            if (quarterToneIndex < 0 || quarterToneIndex > 255)
                return {};

            const int pitchClass = quarterToneIndex % 24;
            const int octave     = quarterToneIndex / 24 - 1;
            const auto& pitch    = kQuarterTonePitchClasses[pitchClass];

            PitchNotation notation;
            notation.valid = true;
            notation.displayLabel = juce::String (pitch.baseName) + pitch.inlineAccidental + juce::String (octave);
            notation.accidentalForStaff = pitch.staffAccidental;
            notation.diatonicNumber = octave * 7 + pitch.degree;
            return notation;
        }

        const float midiFloat = 69.0f + 12.0f * std::log2 (freqHz / 440.0f);
        const int midiNote = (int) std::round (midiFloat);

        if (midiNote < 0 || midiNote > 127)
            return {};

        const int pitchClass = midiNote % 12;
        const int octave     = midiNote / 12 - 1;
        const auto& pitch    = kSemitonePitchClasses[pitchClass];

        PitchNotation notation;
        notation.valid = true;
        notation.displayLabel = juce::String (pitch.baseName) + pitch.inlineAccidental + juce::String (octave);
        notation.accidentalForStaff = pitch.staffAccidental;
        notation.diatonicNumber = octave * 7 + pitch.degree;
        return notation;
    }

    juce::String freqToPitchName (float freqHz, bool useQuarterToneMode)
    {
        const auto notation = quantiseFrequencyToPitchNotation (freqHz, useQuarterToneMode);

        if (! notation.valid)
            return "-";

        return notation.displayLabel;
    }

    std::vector<PitchNotation> chordToPitchNotation (const FrozenChordSnapshot& snapshot)
    {
        std::vector<PitchNotation> notes;
        notes.reserve (snapshot.freqsHz.size());

        for (size_t peakIndex = 0; peakIndex < snapshot.freqsHz.size(); ++peakIndex)
        {
            auto notation = quantiseFrequencyToPitchNotation (snapshot.freqsHz[peakIndex],
                                                               snapshot.useQuarterToneMode);
            if (! notation.valid)
                continue;

            notation.intensity = juce::jlimit (0.0f, 1.0f,
                                                snapshot.partialIntensities[peakIndex]);

            const auto duplicate = std::find_if (notes.begin(), notes.end(),
                                                  [&notation] (const auto& note)
                                                  {
                                                      return note.diatonicNumber == notation.diatonicNumber
                                                          && note.accidentalForStaff == notation.accidentalForStaff;
                                                  });

            if (duplicate != notes.end())
                duplicate->intensity = juce::jmax (duplicate->intensity, notation.intensity);
            else
                notes.push_back (std::move (notation));
        }

        std::sort (notes.begin(), notes.end(),
                   [] (const PitchNotation& a, const PitchNotation& b)
                   {
                       return a.diatonicNumber < b.diatonicNumber;
                   });

        return notes;
    }

    constexpr int kPitchBendCentre = 8192;
    constexpr int kPitchBendRangeSemitones = 1;

    struct QuantisedMidiNote
    {
        bool valid = false;
        int midiNote = -1;
        int pitchWheelValue = kPitchBendCentre;
        float intensity = 1.0f;
    };

    struct MusicXmlPitch
    {
        bool valid = false;
        juce::String step;
        double alter = 0.0;
        juce::String accidental;
        int octave = 0;
        int sortIndex = 0;
    };

    MusicXmlPitch musicXmlPitchForFrequency (float freqHz, bool useQuarterToneMode)
    {
        if (freqHz <= 0.0f)
            return {};

        const int quantisedIndex = useQuarterToneMode
                                     ? (int) std::round (138.0f + 24.0f * std::log2 (freqHz / 440.0f))
                                     : 2 * (int) std::round (69.0f + 12.0f * std::log2 (freqHz / 440.0f));

        if (quantisedIndex < 0 || quantisedIndex > 255)
            return {};

        const int pitchClass = useQuarterToneMode ? quantisedIndex % 24
                                                   : (quantisedIndex / 2) % 12;
        const int octave = useQuarterToneMode ? quantisedIndex / 24 - 1
                                               : (quantisedIndex / 2) / 12 - 1;
        const auto& pitch = useQuarterToneMode ? kQuarterTonePitchClasses[pitchClass]
                                               : kSemitonePitchClasses[pitchClass];

        MusicXmlPitch result;
        result.valid = true;
        result.step = pitch.baseName;
        result.octave = octave;
        result.sortIndex = quantisedIndex;

        if (std::strcmp (pitch.staffAccidental, "#") == 0)
        {
            result.alter = 1.0;
            result.accidental = "sharp";
        }
        else if (std::strcmp (pitch.staffAccidental, "b") == 0)
        {
            result.alter = -1.0;
            result.accidental = "flat";
        }
        else if (std::strcmp (pitch.staffAccidental, "q#") == 0)
        {
            result.alter = 0.5;
            result.accidental = "quarter-sharp";
        }
        else if (std::strcmp (pitch.staffAccidental, "qb") == 0)
        {
            result.alter = -0.5;
            result.accidental = "quarter-flat";
        }

        return result;
    }

    int pitchWheelValueForSemitoneOffset (float semitoneOffset)
    {
        return juce::jlimit (0, 16383,
                             (int) std::round ((double) kPitchBendCentre
                                               + (double) semitoneOffset
                                                 * (double) kPitchBendCentre
                                                 / (double) kPitchBendRangeSemitones));
    }

    int naturalSemitoneForBaseName (const char* baseName)
    {
        switch (baseName[0])
        {
            case 'C': return 0;
            case 'D': return 2;
            case 'E': return 4;
            case 'F': return 5;
            case 'G': return 7;
            case 'A': return 9;
            case 'B': return 11;
            default:  return 0;
        }
    }

    QuantisedMidiNote quantiseFrequencyToMidiNote (float freqHz, bool useQuarterToneMode)
    {
        if (freqHz <= 0.0f)
            return {};

        if (useQuarterToneMode)
        {
            const float quarterToneIndexFloat = 138.0f + 24.0f * std::log2 (freqHz / 440.0f);
            const int quarterToneIndex = (int) std::round (quarterToneIndexFloat);

            if (quarterToneIndex < 0 || quarterToneIndex > 255)
                return {};

            const int pitchClass = quarterToneIndex % 24;
            const int octave     = quarterToneIndex / 24 - 1;
            const auto& pitch    = kQuarterTonePitchClasses[pitchClass];

            int midiNote = 12 * (octave + 1) + naturalSemitoneForBaseName (pitch.baseName);
            float bendSemitones = 0.0f;

            if (std::strcmp (pitch.staffAccidental, "#") == 0)
                ++midiNote;
            else if (std::strcmp (pitch.staffAccidental, "b") == 0)
                --midiNote;
            else if (std::strcmp (pitch.staffAccidental, "q#") == 0)
                bendSemitones = 0.5f;
            else if (std::strcmp (pitch.staffAccidental, "qb") == 0)
                bendSemitones = -0.5f;

            if (! juce::isPositiveAndBelow (midiNote, 128))
                return {};

            return { true, midiNote, pitchWheelValueForSemitoneOffset (bendSemitones) };
        }

        const float midiFloat = 69.0f + 12.0f * std::log2 (freqHz / 440.0f);
        const int midiNote = (int) std::round (midiFloat);
        return juce::isPositiveAndBelow (midiNote, 128)
                 ? QuantisedMidiNote { true, midiNote, kPitchBendCentre }
                 : QuantisedMidiNote {};
    }

    juce::uint8 midiVelocityForPartialIntensity (float intensity)
    {
        const float linearIntensity = juce::jlimit (0.0f, 1.0f, intensity);
        return (juce::uint8) std::round (juce::jmap (linearIntensity,
                                                     (float) kMinimumPartialVelocity,
                                                     127.0f));
    }

    void appendPitchBendRangeSetup (juce::MidiMessageSequence& sequence, int channel, double timeStamp)
    {
        sequence.addEvent (juce::MidiMessage::controllerEvent (channel, 101, 0), timeStamp);
        sequence.addEvent (juce::MidiMessage::controllerEvent (channel, 100, 0), timeStamp);
        sequence.addEvent (juce::MidiMessage::controllerEvent (channel, 6, kPitchBendRangeSemitones), timeStamp);
        sequence.addEvent (juce::MidiMessage::controllerEvent (channel, 38, 0), timeStamp);
        sequence.addEvent (juce::MidiMessage::controllerEvent (channel, 101, 127), timeStamp);
        sequence.addEvent (juce::MidiMessage::controllerEvent (channel, 100, 127), timeStamp);
    }

    constexpr int kMidiTicksPerQuarter = 960;
    constexpr double kMinMidiTimeScale = 1.0;
    constexpr double kMaxMidiTimeScale = 10.0;

    double sanitiseMidiTimeScale (double scale)
    {
        return juce::jlimit (kMinMidiTimeScale, kMaxMidiTimeScale, scale);
    }

    void appendFrozenChordEvents (juce::MidiMessageSequence& eventTrack,
                                  const std::vector<FrozenChordSnapshot>& snapshots,
                                  double timeScale)
    {
        const double exportTimeScale = sanitiseMidiTimeScale (timeScale);

        for (int channel = 1; channel <= AudioPluginAudioProcessor::kNumNoisyPeaks; ++channel)
            appendPitchBendRangeSetup (eventTrack, channel, 0.0);

        for (size_t chordIndex = 0; chordIndex < snapshots.size(); ++chordIndex)
        {
            // Scale every exported timestamp together so notes, pitch bends, and
            // descriptor CCs stay aligned without changing the live analysis.
            const double startBeat = snapshots[chordIndex].startTimeSeconds * exportTimeScale;
            const double endBeat = juce::jmax (snapshots[chordIndex].startTimeSeconds + 0.05,
                                               snapshots[chordIndex].endTimeSeconds)
                                 * exportTimeScale;

            std::vector<QuantisedMidiNote> quantisedNotes;
            quantisedNotes.reserve (snapshots[chordIndex].freqsHz.size());

            for (size_t peakIndex = 0;
                 peakIndex < snapshots[chordIndex].freqsHz.size();
                 ++peakIndex)
            {
                auto midiNote = quantiseFrequencyToMidiNote (
                    snapshots[chordIndex].freqsHz[peakIndex],
                    snapshots[chordIndex].useQuarterToneMode);
                if (! midiNote.valid)
                    continue;

                midiNote.intensity = juce::jlimit (
                    0.0f, 1.0f, snapshots[chordIndex].partialIntensities[peakIndex]);

                const auto duplicate = std::find_if (quantisedNotes.begin(), quantisedNotes.end(),
                                                      [&midiNote] (const auto& note)
                                                      {
                                                          return note.midiNote == midiNote.midiNote
                                                              && note.pitchWheelValue == midiNote.pitchWheelValue;
                                                      });

                if (duplicate != quantisedNotes.end())
                {
                    duplicate->intensity = juce::jmax (duplicate->intensity, midiNote.intensity);
                    continue;
                }

                quantisedNotes.push_back (midiNote);
            }

            if (snapshots[chordIndex].useQuarterToneMode)
            {
                int channel = 1;
                for (const auto& note : quantisedNotes)
                {
                    const auto velocity = midiVelocityForPartialIntensity (note.intensity);
                    eventTrack.addEvent (juce::MidiMessage::pitchWheel (channel, note.pitchWheelValue),
                                         startBeat * kMidiTicksPerQuarter);
                    eventTrack.addEvent (juce::MidiMessage::noteOn (channel, note.midiNote, velocity),
                                         startBeat * kMidiTicksPerQuarter);
                    eventTrack.addEvent (juce::MidiMessage::noteOff (channel, note.midiNote),
                                         endBeat * kMidiTicksPerQuarter);
                    eventTrack.addEvent (juce::MidiMessage::pitchWheel (channel, kPitchBendCentre),
                                         endBeat * kMidiTicksPerQuarter);

                    if (++channel > 16)
                        break;
                }
            }
            else
            {
                for (const auto& note : quantisedNotes)
                {
                    const auto velocity = midiVelocityForPartialIntensity (note.intensity);
                    eventTrack.addEvent (juce::MidiMessage::noteOn (1, note.midiNote, velocity),
                                         startBeat * kMidiTicksPerQuarter);
                    eventTrack.addEvent (juce::MidiMessage::noteOff (1, note.midiNote),
                                         endBeat * kMidiTicksPerQuarter);
                }
            }
        }

        eventTrack.updateMatchedPairs();
    }

    int normalisedDescriptorToCc (float value)
    {
        return juce::jlimit (0, 127, (int) std::round (juce::jlimit (0.0f, 1.0f, value) * 127.0f));
    }

    int centroidToCc (float frequencyHz)
    {
        constexpr float minFrequencyHz = 20.0f;
        constexpr float maxFrequencyHz = 24000.0f;
        const float clampedHz = juce::jlimit (minFrequencyHz, maxFrequencyHz, frequencyHz);
        const float normalised = std::log (clampedHz / minFrequencyHz)
                               / std::log (maxFrequencyHz / minFrequencyHz);
        return normalisedDescriptorToCc (normalised);
    }

    void appendDescriptorEvents (juce::MidiMessageSequence& eventTrack,
                                 const std::vector<DetailAnalysisFrame>& frames,
                                 double timeScale)
    {
        constexpr int midiChannel = 1;
        constexpr int centroidCc = 20;
        constexpr int flatnessCc = 21;
        constexpr int roughnessCc = 22;
        constexpr int stereoPanCc = 23;
        const double exportTimeScale = sanitiseMidiTimeScale (timeScale);

        int previousCentroid = -1;
        int previousFlatness = -1;
        int previousRoughness = -1;
        int previousPan = -1;

        for (const auto& frame : frames)
        {
            const double tick = juce::jmax (0.0, frame.timeSeconds)
                              * exportTimeScale
                              * kMidiTicksPerQuarter;
            const int centroid = centroidToCc (frame.centroidHz);
            const int flatness = normalisedDescriptorToCc (frame.spectralFlatness);
            const int roughness = normalisedDescriptorToCc (frame.roughness);

            if (centroid != previousCentroid)
            {
                eventTrack.addEvent (juce::MidiMessage::controllerEvent (midiChannel, centroidCc, centroid), tick);
                previousCentroid = centroid;
            }

            if (flatness != previousFlatness)
            {
                eventTrack.addEvent (juce::MidiMessage::controllerEvent (midiChannel, flatnessCc, flatness), tick);
                previousFlatness = flatness;
            }

            if (roughness != previousRoughness)
            {
                eventTrack.addEvent (juce::MidiMessage::controllerEvent (midiChannel, roughnessCc, roughness), tick);
                previousRoughness = roughness;
            }

            if (frame.stereoPanAvailable)
            {
                const int pan = normalisedDescriptorToCc (0.5f * (frame.stereoPanEnergy + 1.0f));
                if (pan != previousPan)
                {
                    eventTrack.addEvent (juce::MidiMessage::controllerEvent (midiChannel, stereoPanCc, pan), tick);
                    previousPan = pan;
                }
            }
        }
    }

    juce::File writeMidiFile (const juce::String& baseName,
                              const juce::MidiMessageSequence& eventTrack)
    {
        juce::MidiMessageSequence combinedTrack (eventTrack);
        combinedTrack.addEvent (juce::MidiMessage::tempoMetaEvent (1000000), 0.0); // 60 BPM: one beat = one second
        combinedTrack.addEvent (juce::MidiMessage::timeSignatureMetaEvent (4, 4), 0.0);

        juce::MidiFile midiFile;
        midiFile.setTicksPerQuarterNote (kMidiTicksPerQuarter);
        midiFile.addTrack (combinedTrack);

        auto midiFilePath = juce::File::getSpecialLocation (juce::File::tempDirectory)
                                .getNonexistentChildFile (baseName, ".mid", false);

        if (auto stream = midiFilePath.createOutputStream())
            midiFile.writeTo (*stream, 0); // Type 0: one track containing tempo, notes, pitch bend, and CC.

        return midiFilePath;
    }

    juce::File createMidiFileForSnapshots (const std::vector<FrozenChordSnapshot>& snapshots,
                                           double timeScale)
    {
        juce::MidiMessageSequence eventTrack;
        appendFrozenChordEvents (eventTrack, snapshots, timeScale);
        return writeMidiFile ("FrozenChords", eventTrack);
    }

    juce::File createMusicXmlFileForSnapshots (const std::vector<FrozenChordSnapshot>& snapshots)
    {
        // This is a notation-focused pitch export: each detected chord occupies
        // one quarter note, while microtonal accidentals remain explicit.
        juce::String xml;
        xml << "<?xml version=\"1.0\" encoding=\"UTF-8\" standalone=\"no\"?>\n"
            << "<!DOCTYPE score-partwise PUBLIC \"-//Recordare//DTD MusicXML 3.1 Partwise//EN\" "
               "\"http://www.musicxml.org/dtds/partwise.dtd\">\n"
            << "<score-partwise version=\"3.1\">\n"
            << "  <work><work-title>SPEKANA Pitches</work-title></work>\n"
            << "  <identification><encoding><software>SPEKANA</software></encoding></identification>\n"
            << "  <part-list><score-part id=\"P1\"><part-name>SPEKANA</part-name></score-part></part-list>\n"
            << "  <part id=\"P1\">\n";

        int measureNumber = 1;
        int beatInMeasure = 0;

        const auto openMeasure = [&xml, &measureNumber] (bool firstMeasure)
        {
            xml << "    <measure number=\"" << measureNumber << "\">\n";
            if (firstMeasure)
            {
                xml << "      <attributes>\n"
                    << "        <divisions>1</divisions>\n"
                    << "        <key><fifths>0</fifths></key>\n"
                    << "        <time><beats>4</beats><beat-type>4</beat-type></time>\n"
                    << "        <clef><sign>G</sign><line>2</line></clef>\n"
                    << "      </attributes>\n";
            }
        };

        const auto appendQuarterRest = [&xml]()
        {
            xml << "      <note><rest/><duration>1</duration><voice>1</voice><type>quarter</type></note>\n";
        };

        openMeasure (true);

        for (const auto& snapshot : snapshots)
        {
            std::vector<MusicXmlPitch> pitches;
            pitches.reserve (snapshot.freqsHz.size());

            for (float freqHz : snapshot.freqsHz)
            {
                auto pitch = musicXmlPitchForFrequency (freqHz, snapshot.useQuarterToneMode);
                if (pitch.valid)
                    pitches.push_back (std::move (pitch));
            }

            std::sort (pitches.begin(), pitches.end(),
                       [] (const auto& a, const auto& b) { return a.sortIndex < b.sortIndex; });
            pitches.erase (std::unique (pitches.begin(), pitches.end(),
                                        [] (const auto& a, const auto& b)
                                        {
                                            return a.sortIndex == b.sortIndex;
                                        }),
                           pitches.end());

            if (pitches.empty())
            {
                appendQuarterRest();
            }
            else
            {
                for (size_t noteIndex = 0; noteIndex < pitches.size(); ++noteIndex)
                {
                    const auto& pitch = pitches[noteIndex];
                    xml << "      <note>\n";
                    if (noteIndex > 0)
                        xml << "        <chord/>\n";

                    xml << "        <pitch><step>" << pitch.step << "</step>";
                    if (pitch.alter != 0.0)
                        xml << "<alter>" << juce::String (pitch.alter, 1) << "</alter>";
                    xml << "<octave>" << pitch.octave << "</octave></pitch>\n"
                        << "        <duration>1</duration><voice>1</voice><type>quarter</type>\n";

                    if (pitch.accidental.isNotEmpty())
                        xml << "        <accidental>" << pitch.accidental << "</accidental>\n";

                    xml << "      </note>\n";
                }
            }

            if (++beatInMeasure == 4)
            {
                xml << "    </measure>\n";
                ++measureNumber;
                beatInMeasure = 0;
                if (&snapshot != &snapshots.back())
                    openMeasure (false);
            }
        }

        if (beatInMeasure > 0)
        {
            while (beatInMeasure++ < 4)
                appendQuarterRest();
            xml << "    </measure>\n";
        }

        xml << "  </part>\n</score-partwise>\n";

        auto xmlFilePath = juce::File::getSpecialLocation (juce::File::tempDirectory)
                               .getNonexistentChildFile ("SPEKANA_Pitches", ".musicxml", false);

        if (auto stream = xmlFilePath.createOutputStream())
            stream->writeText (xml, false, false, "\n");

        return xmlFilePath;
    }

    juce::File createDescriptorMidiFile (const std::vector<DetailAnalysisFrame>& frames,
                                         double timeScale)
    {
        juce::MidiMessageSequence eventTrack;
        appendDescriptorEvents (eventTrack, frames, timeScale);
        return writeMidiFile ("SPEKANA_Descriptors", eventTrack);
    }

    juce::File createCombinedMidiFile (const std::vector<FrozenChordSnapshot>& snapshots,
                                       const std::vector<DetailAnalysisFrame>& frames,
                                       double timeScale)
    {
        juce::MidiMessageSequence eventTrack;
        appendFrozenChordEvents (eventTrack, snapshots, timeScale);
        appendDescriptorEvents (eventTrack, frames, timeScale);
        eventTrack.updateMatchedPairs();
        return writeMidiFile ("SPEKANA_Combined", eventTrack);
    }
}

class SensitivityControl : public juce::Component
{
public:
    float getSensitivity() const noexcept
    {
        return juce::jmap (sensitivityPosition, 1.0f, 8.0f);
    }

    double getMinGapMs() const noexcept
    {
        return kAutoMinimumGapMs;
    }

    void paint (juce::Graphics& g) override
    {
        auto bounds = getLocalBounds();
        auto title = bounds.removeFromTop (16);
        g.setColour (kTextPrimary);
        g.setFont (makeUIFont (8.6f, true));
        g.drawText ("SENSITIVITY", title, juce::Justification::centredLeft);
        g.setColour (kTextSecondary);
        g.setFont (makeUIFont (8.5f, false));
        g.drawText (juce::String ((int) std::round (sensitivityPosition * 100.0f)) + "%",
                    title,
                    juce::Justification::centredRight);

        const auto track = getTrackBounds();
        const float pointX = track.getX() + sensitivityPosition * track.getWidth();
        const auto neutralTrack = makeOrganicCapsulePath (
            juce::Rectangle<float> (track.getX(), track.getCentreY() - 1.1f, track.getWidth(), 2.2f),
            0.16f);
        g.setColour (kTextPrimary.withAlpha (0.22f));
        g.fillPath (neutralTrack);

        const auto activeTrack = makeOrganicCapsulePath (
            juce::Rectangle<float> (track.getX(),
                                    track.getCentreY() - 1.45f,
                                    juce::jmax (2.0f, pointX - track.getX()),
                                    2.9f),
            0.24f);
        g.setColour (kButtonTerracottaMuted);
        g.fillPath (activeTrack);
        g.fillPath (makeOrganicCapsulePath (
            juce::Rectangle<float> (pointX - 5.0f, track.getCentreY() - 4.0f, 10.0f, 8.0f),
            0.52f));

        auto labels = getLocalBounds().removeFromBottom (12);
        g.setColour (kTextSecondary.withAlpha (0.76f));
        g.setFont (makeUIFont (7.6f, false));
        g.drawText ("Less", labels, juce::Justification::centredLeft);
        g.drawText ("More", labels, juce::Justification::centredRight);
    }

    void mouseDown (const juce::MouseEvent& event) override
    {
        updateFromMouse (event.position);
    }

    void mouseDrag (const juce::MouseEvent& event) override
    {
        updateFromMouse (event.position);
    }

private:
    float sensitivityPosition = 5.0f / 7.0f; // Default sensitivity: 6.0.

    juce::Rectangle<float> getTrackBounds() const
    {
        auto track = getLocalBounds().toFloat().reduced (3.0f, 0.0f);
        track.removeFromTop (19.0f);
        track.removeFromBottom (12.0f);
        return track;
    }

    void updateFromMouse (juce::Point<float> position)
    {
        const auto plot = getTrackBounds();
        sensitivityPosition = juce::jlimit (0.0f, 1.0f,
                                            (position.x - plot.getX()) / plot.getWidth());
        repaint();
    }
};

class CaptureModeSelector : public juce::Component
{
public:
    std::function<void(int)> onSelectionChanged;

    void setSelectedIndex (int newIndex)
    {
        selectedIndex = juce::jlimit (0, 1, newIndex);
        repaint();
    }

    void paint (juce::Graphics& g) override
    {
        const auto bounds = getLocalBounds().toFloat();
        const auto outerPath = makeOrganicCapsulePath (bounds, 0.20f);
        g.setColour (juce::Colour::fromRGB (226, 233, 216).withAlpha (0.20f));
        g.fillPath (outerPath);

        const juce::String labels[] { "AUTO", "MANUAL" };
        for (int index = 0; index < 2; ++index)
        {
            const auto segment = getSegmentBounds (index);
            if (index == selectedIndex)
            {
                juce::ColourGradient mossGradient (juce::Colour::fromRGB (137, 157, 113).withAlpha (0.76f),
                                                    segment.getX(),
                                                    segment.getY(),
                                                    juce::Colour::fromRGB (91, 112, 78).withAlpha (0.72f),
                                                    segment.getRight(),
                                                    segment.getBottom(),
                                                    false);
                g.setGradientFill (mossGradient);
                g.fillPath (makeOrganicCapsulePath (segment.toFloat().reduced (1.0f), 0.34f));
            }

            g.setColour (index == selectedIndex ? kAmberText : kTextPrimary);
            g.setFont (makeUIFont (10.0f, index == selectedIndex));
            g.drawText (labels[index], segment, juce::Justification::centred);
        }

    }

    void mouseDown (const juce::MouseEvent& event) override
    {
        const int newIndex = event.x < getWidth() / 2 ? 0 : 1;
        if (newIndex == selectedIndex)
            return;
        selectedIndex = newIndex;
        repaint();
        if (onSelectionChanged)
            onSelectionChanged (selectedIndex);
    }

private:
    juce::Rectangle<int> getSegmentBounds (int index) const
    {
        const int middle = getWidth() / 2;
        return index == 0 ? juce::Rectangle<int> (0, 0, middle, getHeight())
                          : juce::Rectangle<int> (middle, 0, getWidth() - middle, getHeight());
    }

    int selectedIndex = 0;
};

class TuningSelector : public juce::Component
{
public:
    std::function<void(bool)> onQuarterToneChanged;

    void setQuarterToneEnabled (bool shouldUseQuarterTone)
    {
        quarterToneEnabled = shouldUseQuarterTone;
        repaint();
    }

    void paint (juce::Graphics& g) override
    {
        auto bounds = getLocalBounds();
        auto control = bounds.toFloat();
        const auto outerPath = makeOrganicCapsulePath (control, 0.18f);
        g.setColour (juce::Colours::white.withAlpha (0.24f));
        g.fillPath (outerPath);

        for (int index = 0; index < 2; ++index)
        {
            const auto segment = getSegmentBounds (index);
            const bool selected = quarterToneEnabled == (index == 1);
            if (selected)
            {
                juce::ColourGradient mossGradient (juce::Colour::fromRGB (137, 157, 113).withAlpha (0.76f),
                                                    segment.getX(),
                                                    segment.getY(),
                                                    juce::Colour::fromRGB (91, 112, 78).withAlpha (0.72f),
                                                    segment.getRight(),
                                                    segment.getBottom(),
                                                    false);
                g.setGradientFill (mossGradient);
                g.fillPath (makeOrganicCapsulePath (segment.toFloat().reduced (1.0f), 0.28f));
            }
            g.setColour (selected ? kAmberText : kTextPrimary);
            g.setFont (makeUIFont (9.2f, selected));
            g.drawText (index == 0 ? "SEMITONE" : "QUARTER-TONE",
                        segment,
                        juce::Justification::centred);
        }
    }

    void mouseDown (const juce::MouseEvent& event) override
    {
        const bool newQuarterTone = event.x >= getWidth() / 2;
        if (newQuarterTone == quarterToneEnabled)
            return;
        quarterToneEnabled = newQuarterTone;
        repaint();
        if (onQuarterToneChanged)
            onQuarterToneChanged (quarterToneEnabled);
    }

private:
    juce::Rectangle<int> getSegmentBounds (int index) const
    {
        const int middle = getWidth() / 2;
        return index == 0 ? juce::Rectangle<int> (0, 0, middle, getHeight())
                          : juce::Rectangle<int> (middle, 0, getWidth() - middle, getHeight());
    }

    bool quarterToneEnabled = false;
};

class FrozenChordStaffComponent : public juce::Component,
                                  private juce::ScrollBar::Listener
{
public:
    FrozenChordStaffComponent()
        : horizontalScrollBar (false)
    {
        addAndMakeVisible (horizontalScrollBar);
        horizontalScrollBar.setAutoHide (true);
        horizontalScrollBar.setSingleStepSize (1.0);
        horizontalScrollBar.setColour (juce::ScrollBar::backgroundColourId,
                                       juce::Colours::transparentBlack);
        horizontalScrollBar.setColour (juce::ScrollBar::thumbColourId,
                                       kTextPrimary.withAlpha (0.22f));
        horizontalScrollBar.setColour (juce::ScrollBar::trackColourId,
                                       kTextPrimary.withAlpha (0.06f));
        horizontalScrollBar.addListener (this);
        updateScrollBar (true);
    }

    ~FrozenChordStaffComponent() override
    {
        horizontalScrollBar.removeListener (this);
    }

    void setSnapshots (std::vector<FrozenChordSnapshot> newSnapshots)
    {
        const double previousLatestStart = juce::jmax (0.0,
                                                       (double) snapshots.size()
                                                         - (double) kVisibleFrozenChords);
        const bool shouldFollowLatest = snapshots.empty()
                                     || horizontalScrollBar.getCurrentRangeStart()
                                          >= previousLatestStart - 0.5;

        snapshots = std::move (newSnapshots);
        updateScrollBar (shouldFollowLatest);
        repaint();
    }

    void setMidiTimeScale (double newTimeScale)
    {
        midiTimeScale = sanitiseMidiTimeScale (newTimeScale);
    }

    void paint (juce::Graphics& g) override
    {
        auto bounds = getLocalBounds().reduced (10);
        if (horizontalScrollBar.isVisible())
            bounds.removeFromBottom (12);

        auto header = bounds.removeFromTop (26);

        header.removeFromLeft (72);

        dragMidiBounds = header.removeFromRight (134).translated (0, 6).reduced (2, 0);
        g.setColour (snapshots.empty() ? kButtonTerracotta.darker (0.12f).withAlpha (kButtonBackgroundAlpha)
                                       : kButtonTerracotta.withAlpha (kButtonBackgroundAlpha));
        g.fillRoundedRectangle (dragMidiBounds.toFloat(), kUnifiedCornerRadius);
        g.setColour (kButtonText);
        g.setFont (makeUIFont (10.8f, true));
        g.drawText ("Drag MIDI", dragMidiBounds, juce::Justification::centred);

        auto systemBounds = bounds.reduced (0, 2);
        drawContinuousSystem (g, systemBounds);
    }

    void resized() override
    {
        constexpr int scrollBarHeight = 7;
        constexpr int buttonTopInset = 38;
        constexpr int gapAboveButtons = 10;
        horizontalScrollBar.setBounds (12,
                                       getHeight() - buttonTopInset - gapAboveButtons - scrollBarHeight,
                                       juce::jmax (0, getWidth() - 24),
                                       scrollBarHeight);
    }

    void mouseDown (const juce::MouseEvent& event) override
    {
        dragTriggered = false;
        dragCandidate = dragMidiBounds.contains (event.getPosition());
        scrollDragCandidate = ! dragCandidate && horizontalScrollBar.isVisible();
        scrollDragStartX = event.x;
        scrollRangeStartAtDrag = horizontalScrollBar.getCurrentRangeStart();
    }

    void mouseDrag (const juce::MouseEvent& event) override
    {
        if (dragCandidate)
        {
            if (dragTriggered || snapshots.empty() || event.getDistanceFromDragStart() < 6)
                return;

            auto midiFile = createMidiFileForSnapshots (snapshots, midiTimeScale);
            if (! midiFile.existsAsFile())
                return;

            dragTriggered = true;
            juce::DragAndDropContainer::performExternalDragDropOfFiles (
                juce::StringArray { midiFile.getFullPathName() },
                false,
                this);
            return;
        }

        if (! scrollDragCandidate)
            return;

        const float availableWidth = (float) juce::jmax (1, getWidth() - 40);
        const float pixelsPerChord = availableWidth / (float) juce::jmax (1, kVisibleFrozenChords - 1);
        const double chordDelta = (double) (event.x - scrollDragStartX) / (double) pixelsPerChord;
        horizontalScrollBar.setCurrentRangeStart (scrollRangeStartAtDrag - chordDelta,
                                                  juce::sendNotificationSync);
    }

    void mouseUp (const juce::MouseEvent&) override
    {
        dragCandidate = false;
        scrollDragCandidate = false;
    }

    void mouseWheelMove (const juce::MouseEvent&,
                         const juce::MouseWheelDetails& wheel) override
    {
        if (! horizontalScrollBar.isVisible())
            return;

        const float wheelDelta = std::abs (wheel.deltaX) > std::abs (wheel.deltaY)
                                   ? wheel.deltaX
                                   : wheel.deltaY;
        horizontalScrollBar.setCurrentRangeStart (
            horizontalScrollBar.getCurrentRangeStart() - (double) wheelDelta * 4.0,
            juce::sendNotificationSync);
    }

private:
    struct DrawnNote
    {
        float x = 0.0f;
        float y = 0.0f;
        int diatonicNumber = 0;
        float alpha = 1.0f;
    };

    std::vector<FrozenChordSnapshot> snapshots;
    juce::ScrollBar horizontalScrollBar;
    juce::Rectangle<int> dragMidiBounds;
    bool dragCandidate = false;
    bool dragTriggered = false;
    bool scrollDragCandidate = false;
    int scrollDragStartX = 0;
    double scrollRangeStartAtDrag = 0.0;
    double midiTimeScale = 1.0;

    void scrollBarMoved (juce::ScrollBar*, double) override
    {
        repaint();
    }

    void updateScrollBar (bool shouldFollowLatest)
    {
        const double totalSlots = juce::jmax (1.0, (double) snapshots.size());
        const double visibleSlots = juce::jmin ((double) kVisibleFrozenChords, totalSlots);
        const double latestStart = juce::jmax (0.0, totalSlots - visibleSlots);
        const double requestedStart = shouldFollowLatest
                                        ? latestStart
                                        : juce::jmin (horizontalScrollBar.getCurrentRangeStart(),
                                                      latestStart);

        horizontalScrollBar.setRangeLimits (0.0, totalSlots, juce::dontSendNotification);
        horizontalScrollBar.setCurrentRange (requestedStart,
                                             visibleSlots,
                                             juce::dontSendNotification);
        horizontalScrollBar.setVisible (totalSlots > visibleSlots);
    }

    void drawContinuousSystem (juce::Graphics& g, juce::Rectangle<int> area) const
    {
        auto content = area.reduced (10, 20);

        const float lineSpacing = 6.4f;
        constexpr float staffVerticalOffset = 10.0f;
        const float clefX = (float) content.getX() + 8.0f;
        const float left = (float) content.getX() + 48.0f;
        const float right = (float) content.getRight() - 12.0f;
        const float middleCY = (float) content.getCentreY() + staffVerticalOffset;
        const float trebleBottomY = middleCY - lineSpacing;
        const float bassTopY = middleCY + lineSpacing;
        const float trebleTopY = trebleBottomY - lineSpacing * 4.0f;
        const float bassBottomY = bassTopY + lineSpacing * 4.0f;

        g.setColour (kTextPrimary.withAlpha (0.82f));
        for (int i = 0; i < 5; ++i)
        {
            const float trebleY = trebleBottomY - (float) i * lineSpacing;
            const float bassY = bassTopY + (float) i * lineSpacing;
            g.drawHorizontalLine ((int) std::round (trebleY), left, right);
            g.drawHorizontalLine ((int) std::round (bassY), left, right);
        }

        g.drawVerticalLine ((int) std::round (left), trebleTopY, bassBottomY);
        g.drawVerticalLine ((int) std::round (right), trebleTopY, bassBottomY);

        g.setColour (kTextPrimary);
        g.setFont (makeMusicFont (19.2f));
        g.drawText (musicGlyph ((juce::juce_wchar) 0x1D11E),
                    juce::Rectangle<int> ((int) clefX, (int) (trebleTopY - 4.0f), 18, 34),
                    juce::Justification::centred);

        g.setFont (makeMusicFont (14.4f));
        g.drawText (musicGlyph ((juce::juce_wchar) 0x1D122),
                    juce::Rectangle<int> ((int) clefX, (int) (bassTopY - 5.0f), 16, 22),
                    juce::Justification::centred);

        g.setColour (kTextPrimary.withAlpha (0.75f));
        const float braceX = clefX - 8.0f;
        g.drawLine (braceX, trebleTopY, braceX, bassBottomY, 1.6f);
        g.drawLine (braceX, trebleTopY, braceX + 8.0f, trebleTopY, 1.6f);
        g.drawLine (braceX, bassBottomY, braceX + 8.0f, bassBottomY, 1.6f);

        if (snapshots.empty())
        {
            return;
        }

        const float startX = left + 8.0f;
        const float availableWidth = right - startX - 8.0f;
        const float step = availableWidth / (float) juce::jmax (1, kVisibleFrozenChords - 1);
        const double nowSeconds = juce::Time::getMillisecondCounterHiRes() * 0.001;
        const size_t firstVisibleIndex = juce::jmin (
            snapshots.size() - 1,
            (size_t) std::floor (horizontalScrollBar.getCurrentRangeStart() + 0.5));
        const size_t visibleCount = juce::jmin ((size_t) kVisibleFrozenChords,
                                               snapshots.size() - firstVisibleIndex);

        for (size_t visibleIndex = 0; visibleIndex < visibleCount; ++visibleIndex)
        {
            const size_t i = firstVisibleIndex + visibleIndex;
            const float x = startX + step * (float) visibleIndex;
            const auto notes = chordToPitchNotation (snapshots[i]);
            if (notes.empty())
                continue;

            float animationProgress = 1.0f;
            if (snapshots[i].visualCreatedWallTimeSeconds > 0.0)
            {
                const auto elapsedSeconds = nowSeconds - snapshots[i].visualCreatedWallTimeSeconds;
                animationProgress = juce::jlimit (0.0f, 1.0f, (float) (elapsedSeconds / 0.28));
            }

            if (animationProgress < 1.0f)
            {
                const float glowAlpha = (1.0f - animationProgress) * 0.18f;
                auto glowBounds = juce::Rectangle<float> (x - 15.0f,
                                                          (float) content.getY() + 8.0f,
                                                          32.0f,
                                                          (float) content.getHeight() - 16.0f);
                g.setColour (kButtonTerracottaStrong.withAlpha (glowAlpha));
                g.fillRoundedRectangle (glowBounds, kUnifiedCornerRadius);
            }

            drawChord (g, content, notes, x);

            if (visibleIndex + 1 < visibleCount)
            {
                const float barlineX = x + step * 0.5f;
                g.setColour (kTextPrimary.withAlpha (0.20f));
                g.drawVerticalLine ((int) std::round (barlineX), trebleTopY, bassBottomY);
            }
        }
    }

    float yForDiatonicNumber (juce::Rectangle<int> area, int diatonicNumber) const
    {
        constexpr float lineSpacing = 6.4f;
        constexpr float staffVerticalOffset = 10.0f;
        constexpr int middleCDiatonicNumber = 28; // C4
        const float middleCY = (float) area.getCentreY() + staffVerticalOffset;
        const float halfStep = lineSpacing * 0.5f;
        return middleCY - (float) (diatonicNumber - middleCDiatonicNumber) * halfStep;
    }

    void drawLedgerLines (juce::Graphics& g,
                          juce::Rectangle<int> area,
                          float noteX,
                          int diatonicNumber,
                          float alpha) const
    {
        constexpr int bassBottomLine = 18;  // G2
        constexpr int middleC = 28;         // C4
        constexpr int trebleTopLine = 38;   // F5
        const float noteWidth = 10.0f;
        g.setColour (kTextPrimary.withAlpha (alpha));

        if (diatonicNumber == middleC)
        {
            const float y = yForDiatonicNumber (area, middleC);
            g.drawLine (noteX - 5.0f, y, noteX + noteWidth + 1.0f, y, 1.0f);
        }

        if (diatonicNumber > trebleTopLine)
        {
            for (int ledger = 40; ledger <= diatonicNumber + (diatonicNumber % 2); ledger += 2)
            {
                const float y = yForDiatonicNumber (area, ledger);
                g.drawLine (noteX - 5.0f, y, noteX + noteWidth + 1.0f, y, 1.0f);
            }
        }
        else if (diatonicNumber < bassBottomLine)
        {
            for (int ledger = 16; ledger >= diatonicNumber - (diatonicNumber % 2 == 0 ? 0 : 1); ledger -= 2)
            {
                const float y = yForDiatonicNumber (area, ledger);
                g.drawLine (noteX - 5.0f, y, noteX + noteWidth + 1.0f, y, 1.0f);
            }
        }
    }

    void drawChord (juce::Graphics& g,
                    juce::Rectangle<int> area,
                    const std::vector<PitchNotation>& notes,
                    float centerX) const
    {
        const float noteHeadWidth = 5.8f;
        const float noteHeadHeight = 3.9f;

        int previousDiatonic = std::numeric_limits<int>::min();
        int clusterIndex = 0;
        std::vector<DrawnNote> drawnNotes;
        drawnNotes.reserve (notes.size());

        for (const auto& note : notes)
        {
            if (note.diatonicNumber - previousDiatonic > 1)
                clusterIndex = 0;

            const float xOffset = (clusterIndex % 2 == 0) ? 0.0f : 2.8f;
            const float noteX = centerX + xOffset;
            const float noteY = yForDiatonicNumber (area, note.diatonicNumber);
            // The detected peak level is already linearly normalised from the
            // fixed dBFS calibration range. Keep notation linear as well; the
            // small alpha floor preserves very quiet detected notes.
            const float noteAlpha = juce::jmap (
                juce::jlimit (0.0f, 1.0f, note.intensity),
                0.06f,
                1.0f);

            g.setColour (kTextPrimary.withAlpha (noteAlpha));
            drawLedgerLines (g, area, noteX, note.diatonicNumber, noteAlpha);

            if (note.accidentalForStaff == "q#" || note.accidentalForStaff == "qb")
            {
                g.setColour (kTextPrimary.withAlpha (noteAlpha));
                drawQuarterToneArrowMark (g,
                                          juce::Rectangle<float> (noteX - 10.0f, noteY - 5.5f, 6.0f, 11.0f),
                                          note.accidentalForStaff == "q#");
            }
            else
            {
                const auto accidentalGlyph = accidentalGlyphForStaff (note.accidentalForStaff);
                if (accidentalGlyph.isNotEmpty())
                {
                    g.setColour (kTextPrimary.withAlpha (noteAlpha));
                    g.setFont (makeMusicFont (8.0f));
                    g.drawText (accidentalGlyph,
                                juce::Rectangle<float> (noteX - 10.0f, noteY - 5.0f, 8.0f, 10.0f).toNearestInt(),
                                juce::Justification::centredRight);
                }
            }

            juce::Path noteHead;
            noteHead.addEllipse (noteX, noteY - noteHeadHeight * 0.5f, noteHeadWidth, noteHeadHeight);
            noteHead.applyTransform (juce::AffineTransform::rotation (juce::degreesToRadians (-22.0f),
                                                                      noteX + noteHeadWidth * 0.5f,
                                                                      noteY));
            g.setColour (kTextPrimary.withAlpha (noteAlpha));
            g.fillPath (noteHead);

            drawnNotes.push_back ({ noteX, noteY, note.diatonicNumber, noteAlpha });

            previousDiatonic = note.diatonicNumber;
            ++clusterIndex;
        }

        if (drawnNotes.empty())
            return;

        const float stemAlpha = std::max_element (drawnNotes.begin(), drawnNotes.end(),
                                                   [] (const auto& a, const auto& b)
                                                   {
                                                       return a.alpha < b.alpha;
                                                   })->alpha;
        const bool stemDown = drawnNotes.back().diatonicNumber >= 34;

        if (stemDown)
        {
            const auto& anchor = drawnNotes.back();
            const float stemX = anchor.x + 0.8f;
            g.setColour (kTextPrimary.withAlpha (stemAlpha));
            g.drawLine (stemX, anchor.y, stemX, anchor.y + 14.4f, 1.0f);
        }
        else
        {
            const auto& anchor = drawnNotes.front();
            const float stemX = anchor.x + noteHeadWidth - 0.8f;
            g.setColour (kTextPrimary.withAlpha (stemAlpha));
            g.drawLine (stemX, anchor.y, stemX, anchor.y - 14.4f, 1.0f);
        }
    }
};

class DescriptorMidiDragComponent : public juce::Component
{
public:
    DescriptorMidiDragComponent (const std::vector<DetailAnalysisFrame>& analysisFrames,
                                 const std::vector<FrozenChordSnapshot>& chordSnapshots)
        : frames (analysisFrames),
          snapshots (chordSnapshots)
    {
        setMouseCursor (juce::MouseCursor::DraggingHandCursor);
    }

    void setMidiTimeScale (double newTimeScale)
    {
        midiTimeScale = sanitiseMidiTimeScale (newTimeScale);
    }

    void paint (juce::Graphics& g) override
    {
        const auto bounds = getLocalBounds().toFloat();
        const bool enabled = ! frames.empty();
        g.setColour (enabled ? kPanelTint.withAlpha (0.78f)
                             : kButtonTerracotta.darker (0.08f).withAlpha (0.64f));
        g.fillRoundedRectangle (bounds, kUnifiedCornerRadius);

        const auto icon = juce::Rectangle<float> (bounds.getX() + 10.0f,
                                                  bounds.getCentreY() - 6.0f,
                                                  9.0f,
                                                  12.0f);
        juce::Path fileIcon;
        fileIcon.startNewSubPath (icon.getX(), icon.getY());
        fileIcon.lineTo (icon.getRight() - 3.0f, icon.getY());
        fileIcon.lineTo (icon.getRight(), icon.getY() + 3.0f);
        fileIcon.lineTo (icon.getRight(), icon.getBottom());
        fileIcon.lineTo (icon.getX(), icon.getBottom());
        fileIcon.closeSubPath();
        fileIcon.startNewSubPath (icon.getRight() - 3.0f, icon.getY());
        fileIcon.lineTo (icon.getRight() - 3.0f, icon.getY() + 3.0f);
        fileIcon.lineTo (icon.getRight(), icon.getY() + 3.0f);

        g.setColour ((enabled ? kButtonTerracottaMuted : kTextSecondary).withAlpha (0.86f));
        g.strokePath (fileIcon, juce::PathStrokeType (0.9f));
        g.drawLine (icon.getX() + 2.0f, icon.getBottom() - 4.0f,
                    icon.getRight() - 2.0f, icon.getBottom() - 4.0f, 0.8f);

        g.setColour (enabled ? kTextPrimary : kTextSecondary);
        g.setFont (makeUIFont (8.4f, false));
        g.drawFittedText (snapshots.empty() ? "Drag Descriptor MIDI" : "Drag Combined MIDI",
                          getLocalBounds().withTrimmedLeft (25).withTrimmedRight (7),
                          juce::Justification::centredLeft,
                          1);

        g.setColour (kTextPrimary.withAlpha (enabled ? 0.18f : 0.10f));
        g.drawRoundedRectangle (bounds.reduced (0.5f), kUnifiedCornerRadius, 0.8f);
    }

    void mouseDown (const juce::MouseEvent&) override
    {
        dragTriggered = false;
    }

    void mouseDrag (const juce::MouseEvent& event) override
    {
        if (dragTriggered || frames.empty() || event.getDistanceFromDragStart() < 6)
            return;

        const auto midiFile = snapshots.empty()
                                ? createDescriptorMidiFile (frames, midiTimeScale)
                                : createCombinedMidiFile (snapshots, frames, midiTimeScale);
        if (! midiFile.existsAsFile())
            return;

        dragTriggered = true;
        juce::DragAndDropContainer::performExternalDragDropOfFiles (
            juce::StringArray { midiFile.getFullPathName() }, false, this);
    }

private:
    const std::vector<DetailAnalysisFrame>& frames;
    const std::vector<FrozenChordSnapshot>& snapshots;
    bool dragTriggered = false;
    double midiTimeScale = 1.0;
};

class MidiTimeScaleSelector : public juce::Component
{
public:
    MidiTimeScaleSelector()
    {
        setMouseCursor (juce::MouseCursor::PointingHandCursor);
        setTitle ("MIDI Time Scale");
        setDescription ("Choose how much to stretch the exported MIDI timeline");
    }

    double getTimeScale() const noexcept
    {
        return timeScales[(size_t) selectedIndex];
    }

    std::function<void(double)> onTimeScaleChanged;

    void paint (juce::Graphics& g) override
    {
        auto bounds = getLocalBounds();
        g.setColour (kPanelTint.withAlpha (0.42f));
        g.fillRoundedRectangle (bounds.toFloat(), kUnifiedCornerRadius);

        g.setColour (kTextSecondary.withAlpha (0.76f));
        g.setFont (makeUIFont (7.0f, false));
        g.drawText ("TIME SCALE",
                    bounds.removeFromLeft (labelWidth).reduced (7, 0),
                    juce::Justification::centredLeft);

        const auto multiplicationSign = juce::String::charToString ((juce::juce_wchar) 0x00d7);

        for (int index = 0; index < (int) timeScales.size(); ++index)
        {
            const auto segment = getSegmentBounds (index);

            if (index == selectedIndex)
            {
                g.setColour (kButtonTerracottaMuted);
                g.fillRect ((float) segment.getX() + 5.0f,
                            (float) segment.getBottom() - 3.0f,
                            (float) segment.getWidth() - 10.0f,
                            2.0f);
            }

            g.setColour (index == selectedIndex ? kAmberText
                                                : kTextSecondary.withAlpha (0.82f));
            g.setFont (makeUIFont (8.0f, false));
            g.drawText (multiplicationSign + juce::String ((int) timeScales[(size_t) index]),
                        segment,
                        juce::Justification::centred);
        }
    }

    void mouseDown (const juce::MouseEvent& event) override
    {
        if (getWidth() <= labelWidth || event.x < labelWidth)
            return;

        const int newIndex = juce::jlimit (0,
                                           (int) timeScales.size() - 1,
                                           (event.x - labelWidth) * (int) timeScales.size()
                                               / (getWidth() - labelWidth));
        if (newIndex == selectedIndex)
            return;

        selectedIndex = newIndex;
        repaint();

        if (onTimeScaleChanged)
            onTimeScaleChanged (getTimeScale());
    }

private:
    juce::Rectangle<int> getSegmentBounds (int index) const
    {
        const int scaleWidth = juce::jmax (0, getWidth() - labelWidth);
        const int left = labelWidth + index * scaleWidth / (int) timeScales.size();
        const int right = labelWidth + (index + 1) * scaleWidth / (int) timeScales.size();
        return { left, 0, right - left, getHeight() };
    }

    static constexpr int labelWidth = 72;
    static constexpr std::array<double, 5> timeScales { 1.0, 2.0, 4.0, 5.0, 10.0 };
    int selectedIndex = 0;
};

class DetailViewSelector : public juce::Component
{
public:
    DetailViewSelector()
    {
        setMouseCursor (juce::MouseCursor::PointingHandCursor);
        setTitle ("Analysis View");
    }

    std::function<void(int)> onSelectionChanged;

    void setSelectedIndex (int newIndex)
    {
        const int limitedIndex = juce::jlimit (0, 2, newIndex);
        if (selectedIndex == limitedIndex)
            return;

        selectedIndex = limitedIndex;
        repaint();
    }

    void paint (juce::Graphics& g) override
    {
        const juce::String labels[] { "Grid", "Sparklines", "Relational" };
        for (int index = 0; index < 3; ++index)
        {
            const auto segment = getSegmentBounds (index);
            if (index == selectedIndex)
            {
                g.setColour (kButtonTerracottaMuted);
                g.fillRect ((float) segment.getX() + 3.0f,
                            (float) segment.getBottom() - 3.5f,
                            (float) segment.getWidth() - 6.0f,
                            3.0f);
            }

            g.setColour (index == selectedIndex ? kTextPrimary
                                                : kTextSecondary.withAlpha (0.92f));
            g.setFont (makeUIFont (8.2f, index == selectedIndex));
            g.drawText (labels[index], segment, juce::Justification::centred);
        }
    }

    void mouseDown (const juce::MouseEvent& event) override
    {
        if (getWidth() <= 0)
            return;

        const int newIndex = juce::jlimit (0, 2, event.x * 3 / getWidth());
        if (newIndex == selectedIndex)
            return;

        selectedIndex = newIndex;
        repaint();
        if (onSelectionChanged)
            onSelectionChanged (selectedIndex);
    }

private:
    juce::Rectangle<int> getSegmentBounds (int index) const
    {
        const int left = index * getWidth() / 3;
        const int right = (index + 1) * getWidth() / 3;
        return { left, 0, right - left, getHeight() };
    }

    int selectedIndex = 0;
};

namespace
{
    // ====== 频率 <-> Mel scale 工具函数 ======
    constexpr float kMinDisplayFreq = 80.0f;
    constexpr float kMaxDisplayFreq = 5000.0f;

    inline float hzToMel (float hz)
    {
        return 2595.0f * std::log10 (1.0f + hz / 700.0f);
    }

    inline float melToHz (float mel)
    {
        return 700.0f * (std::pow (10.0f, mel / 2595.0f) - 1.0f);
    }
}


//==============================================================================

AudioPluginAudioProcessorEditor::AudioPluginAudioProcessorEditor (AudioPluginAudioProcessor& p)
    : AudioProcessorEditor (&p), processorRef (p)
{
    lookAndFeel = std::make_unique<MinimalPluginLookAndFeel>();
    setLookAndFeel (lookAndFeel.get());

    setSize (kEditorWidth, kEditorHeight);
    spectrogramImage = juce::Image (juce::Image::ARGB, 420, 220, true);
    clearSpectrogramImage();

    startTimerHz (30);

    addAndMakeVisible (freezeButton);
    addAndMakeVisible (unfreezeButton);
    addAndMakeVisible (resetButton);
    addAndMakeVisible (bassBoostButton);
    addAndMakeVisible (exportMidiButton);
    addAndMakeVisible (autoStartButton);
    addAndMakeVisible (autoStopButton);
    addAndMakeVisible (autoClearButton);
    addAndMakeVisible (detailAnalysisButton);
    addAndMakeVisible (topPeakCountSlider);

    captureModeSelector = std::make_unique<CaptureModeSelector>();
    addAndMakeVisible (*captureModeSelector);

    tuningSelector = std::make_unique<TuningSelector>();
    addAndMakeVisible (*tuningSelector);

    sensitivityControl = std::make_unique<SensitivityControl>();
    addAndMakeVisible (*sensitivityControl);

    staffComponent = std::make_unique<FrozenChordStaffComponent>();
    addAndMakeVisible (*staffComponent);

    descriptorMidiDragComponent = std::make_unique<DescriptorMidiDragComponent> (detailAnalysisHistory,
                                                                                  frozenChords);
    addAndMakeVisible (*descriptorMidiDragComponent);
    descriptorMidiDragComponent->setVisible (false);

    midiTimeScaleSelector = std::make_unique<MidiTimeScaleSelector>();
    addAndMakeVisible (*midiTimeScaleSelector);
    midiTimeScaleSelector->setVisible (false);

    detailViewSelector = std::make_unique<DetailViewSelector>();
    addAndMakeVisible (*detailViewSelector);
    detailViewSelector->setVisible (false);

    styleTextButton (freezeButton, true);
    styleTextButton (unfreezeButton);
    styleTextButton (resetButton);
    styleTextButton (bassBoostButton);
    styleTextButton (exportMidiButton);
    styleTextButton (autoStartButton, true);
    styleTextButton (autoStopButton);
    styleTextButton (autoClearButton);
    styleTextButton (detailAnalysisButton);

    for (auto* button : { &freezeButton,
                          &unfreezeButton,
                          &resetButton,
                          &exportMidiButton,
                          &autoStartButton,
                          &autoStopButton,
                          &autoClearButton,
                          &bassBoostButton,
                          &detailAnalysisButton })
        button->getProperties().set (juce::Identifier ("largeButtonText"), true);

    for (auto* button : { &freezeButton,
                          &unfreezeButton,
                          &resetButton,
                          &exportMidiButton,
                          &autoStartButton,
                          &autoStopButton,
                          &autoClearButton })
        button->getProperties().set (juce::Identifier ("fontSizeOffset"), 2.0);

    for (auto* button : { &bassBoostButton,
                          &detailAnalysisButton })
        button->getProperties().set (juce::Identifier ("backgroundOpacity"), 0.71);

    detailAnalysisButton.getProperties().set (juce::Identifier ("textTab"), true);
    bassBoostButton.getProperties().set (juce::Identifier ("parameterToggle"), true);
    autoStartButton.getProperties().set (juce::Identifier ("primaryAction"), true);
    freezeButton.getProperties().set (juce::Identifier ("primaryAction"), true);
    for (auto* button : { &autoStopButton, &autoClearButton, &unfreezeButton, &resetButton })
        button->getProperties().set (juce::Identifier ("secondaryAction"), true);
    exportMidiButton.getProperties().set (juce::Identifier ("outlineAction"), true);

    autoStartButton.setButtonText ("START");
    freezeButton.setButtonText ("FREEZE");

    captureModeSelector->onSelectionChanged = [this] (int selectedIndex)
    {
        setCaptureMode (selectedIndex == 0 ? CaptureMode::Auto : CaptureMode::Manual);
    };

    tuningSelector->onQuarterToneChanged = [this] (bool shouldUseQuarterTone)
    {
        quarterToneMode = shouldUseQuarterTone;
        repaint();
    };

    autoStartButton.onClick = [this]()
    {
        if (isAutoRunning)
        {
            const double nowSeconds = juce::Time::getMillisecondCounterHiRes() * 0.001;
            finishAutoAnalysis (nowSeconds, "Auto stopped.", false);
            return;
        }

        isDetailPageActive = false;
        isAutoRunning = true;
        autoStartWallTimeSeconds = juce::Time::getMillisecondCounterHiRes() * 0.001;
        previousHostTransportPlaying = processorRef.hasHostTransportState()
                                         && processorRef.getHostTransportPlaying();
        hasObservedHostPlaybackSinceAutoStart = previousHostTransportPlaying;
        detailAnalysisHistory.clear();
        hasCompletedAutoAnalysis = false;
        resetAutoDetectorState();
        refreshAutoButtonStates();
        refreshDetailAnalysisButtonState();
        refreshExportButtonState();
        showTransientStatusMessage ("Auto listening for onsets.");
        repaint();
    };

    autoClearButton.onClick = [this]()
    {
        resetFrozenState();
        resetAutoDetectorState();
        showTransientStatusMessage ("Auto captures cleared.");
    };

    detailAnalysisButton.setClickingTogglesState (true);
    detailAnalysisButton.onClick = [this]()
    {
        if (! hasCompletedAutoAnalysis || detailAnalysisHistory.empty() || isAutoRunning)
        {
            detailAnalysisButton.setToggleState (false, juce::dontSendNotification);
            return;
        }

        isDetailPageActive = ! isDetailPageActive;
        detailAnalysisButton.setToggleState (isDetailPageActive, juce::dontSendNotification);

        if (staffComponent != nullptr)
            staffComponent->setVisible (! isDetailPageActive);

        if (descriptorMidiDragComponent != nullptr)
            descriptorMidiDragComponent->setVisible (isDetailPageActive);

        if (midiTimeScaleSelector != nullptr)
            midiTimeScaleSelector->setVisible (isDetailPageActive);

        if (detailViewSelector != nullptr)
            detailViewSelector->setVisible (isDetailPageActive);
        resized();

        repaint();
    };

    const auto selectDetailView = [this] (DetailViewMode mode)
    {
        detailViewMode = mode;
        if (detailViewSelector != nullptr)
            detailViewSelector->setSelectedIndex ((int) mode);
        analysisHoverTarget = -1;
        repaint();
    };

    detailViewSelector->onSelectionChanged = [selectDetailView] (int selectedIndex)
    {
        selectDetailView ((DetailViewMode) juce::jlimit (0, 2, selectedIndex));
    };
    selectDetailView (DetailViewMode::grid);

    topPeakCountSlider.setRange (1.0, (double) kNumNoisyPeaks, 1.0);
    topPeakCountSlider.setValue ((double) kNumNoisyPeaks, juce::dontSendNotification);
    topPeakCountSlider.setSliderStyle (juce::Slider::LinearHorizontal);
    topPeakCountSlider.setTextBoxStyle (juce::Slider::TextBoxRight, false, 36, 19);
    topPeakCountSlider.setColour (juce::Slider::trackColourId, kButtonTerracotta);
    topPeakCountSlider.setColour (juce::Slider::thumbColourId, kButtonTerracottaStrong);
    topPeakCountSlider.setColour (juce::Slider::backgroundColourId, kPanelTint.withAlpha (0.63f));
    topPeakCountSlider.setColour (juce::Slider::textBoxTextColourId, kTextPrimary);
    topPeakCountSlider.setColour (juce::Slider::textBoxOutlineColourId, juce::Colours::transparentBlack);
    topPeakCountSlider.onValueChange = [this]()
    {
        repaint();
    };

    midiTimeScaleSelector->onTimeScaleChanged = [this] (double timeScale)
    {
        if (staffComponent != nullptr)
            staffComponent->setMidiTimeScale (timeScale);

        if (descriptorMidiDragComponent != nullptr)
            descriptorMidiDragComponent->setMidiTimeScale (timeScale);
    };

    bassBoostButton.setClickingTogglesState (true);
    bassBoostButton.setToggleState (processorRef.getBassBoostMode(), juce::dontSendNotification);
    refreshBassBoostButtonText();
    bassBoostButton.onClick = [this]()
    {
        processorRef.setBassBoostMode (bassBoostButton.getToggleState());
        refreshBassBoostButtonText();
        repaint();
    };

freezeButton.onClick = [this]()
{
    const double nowSeconds = juce::Time::getMillisecondCounterHiRes() * 0.001;

    if (! isFrozen)
    {
        activeFreezeSessionStartWallTimeSeconds = nowSeconds;
        activeFreezeSessionOffsetSeconds = frozenChords.empty() ? 0.0 : frozenChords.back().endTimeSeconds;
    }
    else
    {
        closeActiveFrozenChord();
    }

    // enter freeze mode
    isFrozen = true;
    freezeButton.setToggleState (true, juce::dontSendNotification);

    spectrumForDrawing = latestSpectrum;
    topFreqsForDrawing = latestTopFreqs;
    topMagsForDrawing  = latestTopMags;
    topPeakLevelsDbFsForDrawing = latestTopPeakLevelsDbFs;

    pendingFreezeMarker = true;
    pushSpectrumToImage();
    captureFrozenChord();

    repaint();
};



unfreezeButton.onClick = [this]()
{
    closeActiveFrozenChord();
    isFrozen = false;
    freezeButton.setToggleState (false, juce::dontSendNotification);
    processorRef.clearLiveFrozenMidiChord();
    repaint();
};

    resetButton.onClick = [this]()
    {
        resetFrozenState();
    };

    exportMidiButton.onClick = [this]()
    {
        juce::PopupMenu exportMenu;
        exportMenu.setLookAndFeel (lookAndFeel.get());
        exportMenu.addItem (1, "MIDI");
        exportMenu.addItem (2, "MusicXML");

        auto exportAnchor = exportMidiButton.getScreenBounds();
        exportAnchor.setX (exportAnchor.getCentreX());
        exportAnchor.setWidth (1);

        exportMenu.showMenuAsync (juce::PopupMenu::Options()
                                      .withTargetComponent (exportMidiButton)
                                      .withTargetScreenArea (exportAnchor)
                                      .withPreferredPopupDirection (
                                          juce::PopupMenu::Options::PopupDirection::downwards)
                                      .withMinimumWidth (112)
                                      .withStandardItemHeight (24),
                                  [this] (int result)
                                  {
                                      if (result == 0)
                                          return;

                                      const bool exportMusicXml = result == 2;
                                      const double timeScale = midiTimeScaleSelector != nullptr
                                                                 ? midiTimeScaleSelector->getTimeScale()
                                                                 : 1.0;
                                      const auto sourceFile = exportMusicXml
                                                                ? createMusicXmlFileForSnapshots (frozenChords)
                                                                : createMidiFileForSnapshots (frozenChords,
                                                                                              timeScale);
                                      if (! sourceFile.existsAsFile())
                                          return;

                                      const juce::String extension = exportMusicXml ? ".musicxml" : ".mid";
                                      const juce::String wildcard = exportMusicXml ? "*.musicxml;*.xml" : "*.mid";
                                      const juce::String title = exportMusicXml
                                                                   ? "Export Pitch MusicXML"
                                                                   : "Export Pitch MIDI";

                                      exportFileChooser = std::make_unique<juce::FileChooser> (
                                          title,
                                          juce::File::getSpecialLocation (juce::File::userDocumentsDirectory)
                                              .getNonexistentChildFile ("SPEKANA_Pitches", extension, false),
                                          wildcard);

                                      exportFileChooser->launchAsync (
                                          juce::FileBrowserComponent::saveMode
                                            | juce::FileBrowserComponent::canSelectFiles,
                                          [this, sourceFile, extension] (const juce::FileChooser& chooser)
                                          {
                                              auto target = chooser.getResult();
                                              if (target == juce::File())
                                              {
                                                  exportFileChooser.reset();
                                                  return;
                                              }

                                              if (target.getFileExtension().isEmpty())
                                                  target = target.withFileExtension (extension);

                                              if (target.existsAsFile())
                                                  target.deleteFile();

                                              sourceFile.copyFileTo (target);
                                              exportFileChooser.reset();
                                          });
                                  });
    };

    refreshExportButtonState();
    refreshCaptureModeControls();
    refreshAutoButtonStates();
    refreshDetailAnalysisButtonState();
    bassBoostButton.toFront (false);
    resized();
}


AudioPluginAudioProcessorEditor::~AudioPluginAudioProcessorEditor()
{
    setLookAndFeel (nullptr);
}

bool AudioPluginAudioProcessorEditor::useQuarterToneMode() const
{
    return quarterToneMode;
}

bool AudioPluginAudioProcessorEditor::isAutoMode() const noexcept
{
    return currentMode == CaptureMode::Auto;
}

int AudioPluginAudioProcessorEditor::getActivePeakCount() const
{
    return juce::jlimit (1, kNumNoisyPeaks, (int) std::round (topPeakCountSlider.getValue()));
}

juce::String AudioPluginAudioProcessorEditor::formatTimeLabel (double seconds) const
{
    if (seconds < 0.0)
        seconds = 0.0;

    const int totalMilliseconds = (int) std::round (seconds * 1000.0);
    const int minutes = totalMilliseconds / 60000;
    const int secondsPart = (totalMilliseconds / 1000) % 60;
    const int milliseconds = totalMilliseconds % 1000;

    return juce::String (minutes).paddedLeft ('0', 2)
         + ":" + juce::String (secondsPart).paddedLeft ('0', 2)
         + "." + juce::String (milliseconds).paddedLeft ('0', 3);
}

juce::String AudioPluginAudioProcessorEditor::getCurrentHostTimeLabel() const
{
    if (auto* playHead = processorRef.getPlayHead())
    {
        if (const auto position = playHead->getPosition())
        {
            if (const auto timeInSeconds = position->getTimeInSeconds())
                return formatTimeLabel (*timeInSeconds);
        }
    }

    return "local " + formatTimeLabel (activeFreezeSessionOffsetSeconds);
}

void AudioPluginAudioProcessorEditor::setCaptureMode (CaptureMode mode)
{
    if (mode == currentMode)
        return;

    if (mode == CaptureMode::Manual && isAutoRunning)
    {
        const double nowSeconds = juce::Time::getMillisecondCounterHiRes() * 0.001;
        finishAutoAnalysis (nowSeconds, "Auto stopped.", true);
    }

    currentMode = mode;
    isDetailPageActive = false;
    refreshCaptureModeControls();
    refreshDetailAnalysisButtonState();
    refreshStaffComponent();
    resized();
    repaint();
}

void AudioPluginAudioProcessorEditor::refreshCaptureModeControls()
{
    const bool autoMode = isAutoMode();

    if (captureModeSelector != nullptr)
        captureModeSelector->setSelectedIndex (autoMode ? 0 : 1);
    if (tuningSelector != nullptr)
    {
        tuningSelector->setQuarterToneEnabled (quarterToneMode);
        tuningSelector->setVisible (true);
    }

    freezeButton.setVisible (! autoMode);
    unfreezeButton.setVisible (! autoMode);
    resetButton.setVisible (! autoMode);
    exportMidiButton.setVisible (true);

    autoStartButton.setVisible (autoMode);
    autoStopButton.setVisible (false);
    autoClearButton.setVisible (autoMode);
    if (sensitivityControl != nullptr)
        sensitivityControl->setVisible (autoMode);
}

void AudioPluginAudioProcessorEditor::refreshAutoButtonStates()
{
    autoStartButton.setToggleState (isAutoRunning, juce::dontSendNotification);
    autoStartButton.setButtonText (isAutoRunning ? "STOP" : "START");
    autoStartButton.setEnabled (true);
    autoClearButton.setEnabled (! frozenChords.empty() || autoCaptureCount > 0);
}

void AudioPluginAudioProcessorEditor::resetAutoDetectorState()
{
    previousAutoResidual.fill (0.0f);
    previousDetailSpectrum.fill (0.0f);
    hasPreviousAutoResidual = false;
    hasPreviousAutoInputLevel = false;
    hasPreviousDetailSpectrum = false;
    autoFluxMean = 0.0f;
    autoFluxDeviation = 0.18f;
    autoOnsetStrength = 0.0f;
    previousAutoInputLevelDb = -120.0f;
    lastAutoOnsetWallTimeSeconds = -10.0;
}

void AudioPluginAudioProcessorEditor::finishAutoAnalysis (double nowSeconds,
                                                          const juce::String& statusMessage,
                                                          bool clearLiveChord)
{
    if (! frozenChords.empty() && frozenChords.back().label.startsWith ("Auto "))
        frozenChords.back().endTimeSeconds = juce::jmax (frozenChords.back().startTimeSeconds + 0.05,
                                                         nowSeconds - autoStartWallTimeSeconds);

    isAutoRunning = false;
    hasCompletedAutoAnalysis = ! detailAnalysisHistory.empty();

    if (clearLiveChord)
        processorRef.clearLiveFrozenMidiChord();

    refreshStaffComponent();
    refreshAutoButtonStates();
    refreshDetailAnalysisButtonState();
    showTransientStatusMessage (statusMessage);
    repaint();
}

DetailAnalysisFrame AudioPluginAudioProcessorEditor::calculateDetailAnalysisFrame (double nowSeconds)
{
    DetailAnalysisFrame frame;
    frame.timeSeconds = juce::jmax (0.0, nowSeconds - autoStartWallTimeSeconds);
    frame.rmsDb = processorRef.getInputLevelDb();
    frame.stereoPanAvailable = processorRef.getStereoPanAvailable();
    frame.stereoPanEnergy = processorRef.getStereoPanEnergy();

    const float sampleRate = (float) processorRef.getSampleRate();
    const int numBins = (int) latestSpectrum.size();
    if (sampleRate <= 0.0f || numBins <= 2)
        return frame;

    // Descriptor analysis uses the complete available positive-frequency
    // spectrum, independently of the narrower pitch-detection range.
    // Bin 0 is excluded so that DC offset cannot pull the centroid downward.
    const int minBin = 1;
    const int maxBin = numBins - 1;

    double logMagnitudeSum = 0.0;
    double fullMagnitudeSum = 0.0;
    int magnitudeCount = 0;
    float peakSpectrumDb = -120.0f;
    std::array<float, kFftSize / 2> normalisedSpectrum {};

    for (int bin = minBin; bin <= maxBin; ++bin)
    {
        // Keep the lower numerical floor, but do not clamp the upper FFT
        // magnitude. The FFT output is not dBFS-normalised and can exceed
        // +24 dB without representing an invalid audio level.
        const float db = juce::jmax (-120.0f, latestSpectrum[(size_t) bin]);
        const double magnitude = juce::jmax (1.0e-12, std::pow (10.0, (double) db / 20.0));

        normalisedSpectrum[(size_t) bin] = (float) magnitude;
        logMagnitudeSum += std::log (magnitude);
        fullMagnitudeSum += magnitude;
        peakSpectrumDb = juce::jmax (peakSpectrumDb, db);
        ++magnitudeCount;
    }

    if (magnitudeCount > 0 && fullMagnitudeSum > 1.0e-12)
    {
        const double geometricMean = std::exp (logMagnitudeSum / (double) magnitudeCount);
        const double arithmeticMean = fullMagnitudeSum / (double) magnitudeCount;
        frame.spectralFlatness = (float) juce::jlimit (0.0, 1.0, geometricMean / arithmeticMean);

        // Very low-level Hann-window sidelobes can sit far away from a tonal
        // peak. In a second central moment, their large squared distance can
        // otherwise inflate the spread even though they are inaudibly weak.
        // Apply the same peak-relative gate to both centroid and spread so
        // that they remain moments of the same magnitude distribution.
        constexpr float descriptorDynamicRangeDb = 60.0f;
        const float descriptorFloorDb = juce::jmax (-120.0f,
                                                     peakSpectrumDb - descriptorDynamicRangeDb);
        double descriptorMagnitudeSum = 0.0;
        double weightedFrequencySum = 0.0;

        for (int bin = minBin; bin <= maxBin; ++bin)
        {
            const float db = juce::jmax (-120.0f, latestSpectrum[(size_t) bin]);
            if (db <= descriptorFloorDb)
                continue;

            const double magnitude = (double) normalisedSpectrum[(size_t) bin];
            const double hz = (double) bin * sampleRate / (double) kFftSize;
            descriptorMagnitudeSum += magnitude;
            weightedFrequencySum += hz * magnitude;
        }

        if (descriptorMagnitudeSum > 1.0e-12)
        {
            frame.centroidHz = (float) juce::jlimit (0.0, (double) sampleRate * 0.5,
                                                    weightedFrequencySum / descriptorMagnitudeSum);

            // Spectral spread is the magnitude-weighted standard deviation
            // around the gated centroid and remains expressed in hertz.
            double weightedSquaredDeviation = 0.0;
            for (int bin = minBin; bin <= maxBin; ++bin)
            {
                const float db = juce::jmax (-120.0f, latestSpectrum[(size_t) bin]);
                if (db <= descriptorFloorDb)
                    continue;

                const double hz = (double) bin * sampleRate / (double) kFftSize;
                const double distanceFromCentroid = hz - (double) frame.centroidHz;
                weightedSquaredDeviation += (double) normalisedSpectrum[(size_t) bin]
                                          * distanceFromCentroid
                                          * distanceFromCentroid;
            }

            frame.spectralSpreadHz = (float) std::sqrt (weightedSquaredDeviation
                                                        / descriptorMagnitudeSum);
        }
    }

    if (fullMagnitudeSum > 1.0e-12)
    {
        // L1-normalise each spectrum so flux describes spectral-shape motion
        // rather than duplicating the RMS level. Half-wave rectification keeps
        // only bins whose magnitude increased since the previous analysis frame.
        double positiveFlux = 0.0;
        for (int bin = minBin; bin <= maxBin; ++bin)
        {
            const float currentMagnitude = normalisedSpectrum[(size_t) bin]
                                         / (float) fullMagnitudeSum;
            normalisedSpectrum[(size_t) bin] = currentMagnitude;

            if (hasPreviousDetailSpectrum)
                positiveFlux += juce::jmax (0.0f,
                                            currentMagnitude
                                              - previousDetailSpectrum[(size_t) bin]);
        }

        frame.spectralFlux = (float) juce::jlimit (0.0, 1.0, positiveFlux);
    }

    previousDetailSpectrum = normalisedSpectrum;
    hasPreviousDetailSpectrum = true;

    struct Partial
    {
        float hz = 0.0f;
        float amplitude = 0.0f;
    };

    std::array<Partial, kNumNoisyPeaks> partials {};
    int partialCount = 0;

    for (int i = 0; i < getActivePeakCount(); ++i)
    {
        const auto hz = topFreqsForDrawing[(size_t) i];
        if (hz <= 20.0f)
            continue;

        const auto residualDb = juce::jlimit (0.0f, 36.0f, topMagsForDrawing[(size_t) i]);
        partials[(size_t) partialCount++] = { hz, residualDb / 36.0f };

        if (partialCount >= kNumNoisyPeaks)
            break;
    }

    double roughnessSum = 0.0;
    for (int i = 0; i < partialCount; ++i)
    {
        for (int j = i + 1; j < partialCount; ++j)
        {
            const float fMin = juce::jmin (partials[(size_t) i].hz, partials[(size_t) j].hz);
            const float freqDistance = std::abs (partials[(size_t) j].hz - partials[(size_t) i].hz);
            const float s = 0.24f / (0.021f * fMin + 19.0f);
            const float x = s * freqDistance;
            const float pairRoughness = partials[(size_t) i].amplitude
                                      * partials[(size_t) j].amplitude
                                      * (std::exp (-3.5f * x) - std::exp (-5.75f * x));

            roughnessSum += juce::jmax (0.0f, pairRoughness);
        }
    }

    frame.roughness = (float) juce::jlimit (0.0, 1.0, roughnessSum * 2.2);
    return frame;
}

void AudioPluginAudioProcessorEditor::recordDetailAnalysisFrame (double nowSeconds)
{
    if (! isAutoRunning)
        return;

    detailAnalysisHistory.push_back (calculateDetailAnalysisFrame (nowSeconds));

    if ((int) detailAnalysisHistory.size() > kMaxDetailAnalysisFrames)
        detailAnalysisHistory.erase (detailAnalysisHistory.begin());
}

void AudioPluginAudioProcessorEditor::refreshDetailAnalysisButtonState()
{
    const bool canOpenDetail = hasCompletedAutoAnalysis
                            && ! detailAnalysisHistory.empty()
                            && ! isAutoRunning;

    detailAnalysisButton.setEnabled (canOpenDetail);

    if (! canOpenDetail)
        isDetailPageActive = false;

    detailAnalysisButton.setToggleState (isDetailPageActive && canOpenDetail, juce::dontSendNotification);

    if (staffComponent != nullptr)
        staffComponent->setVisible (! isDetailPageActive);

    if (descriptorMidiDragComponent != nullptr)
        descriptorMidiDragComponent->setVisible (isDetailPageActive && canOpenDetail);

    if (midiTimeScaleSelector != nullptr)
        midiTimeScaleSelector->setVisible (isDetailPageActive && canOpenDetail);

    bassBoostButton.setVisible (! isDetailPageActive);

    if (detailViewSelector != nullptr)
    {
        detailViewSelector->setEnabled (canOpenDetail);
        detailViewSelector->setVisible (isDetailPageActive && canOpenDetail);
        detailViewSelector->setSelectedIndex ((int) detailViewMode);
    }
}

void AudioPluginAudioProcessorEditor::captureAutoChord (double nowSeconds)
{
    if (autoCaptureCount >= kMaxAutoChordsSafety)
    {
        finishAutoAnalysis (nowSeconds,
                            "Auto safety limit reached. Press Clear to start a new score.",
                            true);
        return;
    }

    const double startTimeSeconds = juce::jmax (0.0, nowSeconds - autoStartWallTimeSeconds);

    if (! frozenChords.empty() && frozenChords.back().label.startsWith ("Auto "))
        frozenChords.back().endTimeSeconds = juce::jmax (frozenChords.back().startTimeSeconds + 0.05,
                                                         startTimeSeconds);

    FrozenChordSnapshot snapshot;
    if (firstFreezeTimeLabel.isEmpty())
        firstFreezeTimeLabel = getCurrentHostTimeLabel();

    snapshot.freqsHz.fill (0.0f);
    const int activePeakCount = getActivePeakCount();
    for (int i = 0; i < activePeakCount; ++i)
        snapshot.freqsHz[(size_t) i] = topFreqsForDrawing[(size_t) i];
    snapshot.partialIntensities = calculatePartialIntensities (
        snapshot.freqsHz,
        topPeakLevelsDbFsForDrawing,
        activePeakCount);

    snapshot.useQuarterToneMode = useQuarterToneMode();
    snapshot.startTimeSeconds = startTimeSeconds;
    snapshot.endTimeSeconds = startTimeSeconds + 0.25;
    snapshot.visualCreatedWallTimeSeconds = nowSeconds;

    ++autoCaptureCount;
    ++freezeCaptureCount;
    snapshot.label = "Auto " + juce::String (autoCaptureCount);

    frozenChords.push_back (snapshot);
    processorRef.setLiveFrozenMidiChord (snapshot.freqsHz,
                                         snapshot.partialIntensities,
                                         snapshot.useQuarterToneMode);
    pendingFreezeMarker = true;

    refreshStaffComponent();
    refreshAutoButtonStates();
}

void AudioPluginAudioProcessorEditor::processAutoOnsetDetection (double nowSeconds)
{
    if (! isAutoMode() || ! isAutoRunning)
        return;

    const double elapsedSeconds = nowSeconds - autoStartWallTimeSeconds;
    if (elapsedSeconds >= kMaxAutoDurationSeconds)
    {
        finishAutoAnalysis (nowSeconds, "30 second limit reached. Auto stopped.", true);
        return;
    }

    if (autoCaptureCount >= kMaxAutoChordsSafety)
    {
        finishAutoAnalysis (nowSeconds, "Auto safety limit reached.", true);
        return;
    }

    constexpr float kLevelTriggerFloorDb = -72.0f;
    constexpr float kSignificantLevelRiseDb = 6.0f;

    const float inputLevelDb = processorRef.getInputLevelDb();
    bool significantLevelRise = false;

    if (hasPreviousAutoInputLevel)
    {
        significantLevelRise = inputLevelDb > kLevelTriggerFloorDb
                            && inputLevelDb - previousAutoInputLevelDb > kSignificantLevelRiseDb;
    }

    previousAutoInputLevelDb = inputLevelDb;
    hasPreviousAutoInputLevel = true;

    bool spectralOnsetDetected = false;
    float threshold = 0.0f;

    // Keep the existing residual-flux detector unchanged as route A. Route B
    // (the level-rise trigger above) can capture independently of this block.
    if (inputLevelDb > -96.0f && hasPreviousAutoResidual)
    {
        const float sampleRate = (float) processorRef.getSampleRate();

        if (sampleRate > 0.0f)
        {
            const int numBins = (int) latestResidual.size();
            const int minBin = juce::jlimit (1, numBins - 2, (int) std::floor (40.0f * (float) kFftSize / sampleRate));
            const int maxBin = juce::jlimit (minBin + 1, numBins - 2, (int) std::ceil (4000.0f * (float) kFftSize / sampleRate));

            float flux = 0.0f;
            float strongestRise = 0.0f;
            int contributingBins = 0;

            for (int bin = minBin; bin <= maxBin; ++bin)
            {
                const float rise = latestResidual[(size_t) bin] - previousAutoResidual[(size_t) bin];
                strongestRise = juce::jmax (strongestRise, rise);

                if (rise > 0.18f)
                {
                    flux += rise;
                    ++contributingBins;
                }
            }

            if (contributingBins > 0)
                flux /= (float) contributingBins;

            const float instantOnsetStrength = 0.68f * flux + 0.32f * strongestRise;
            autoOnsetStrength = 0.58f * autoOnsetStrength + 0.42f * instantOnsetStrength;

            const float previousMean = autoFluxMean;
            autoFluxMean = 0.985f * autoFluxMean + 0.015f * autoOnsetStrength;
            autoFluxDeviation = 0.985f * autoFluxDeviation
                              + 0.015f * std::abs (autoOnsetStrength - previousMean);

            const float sensitivity = sensitivityControl != nullptr
                                        ? sensitivityControl->getSensitivity()
                                        : 6.0f;
            const float thresholdScale = juce::jmap (sensitivity, 1.0f, 8.0f, 3.2f, 1.1f);
            threshold = autoFluxMean + thresholdScale * juce::jmax (0.18f, autoFluxDeviation);
            spectralOnsetDetected = autoOnsetStrength > threshold && autoOnsetStrength > 0.28f;
        }
    }

    previousAutoResidual = latestResidual;
    hasPreviousAutoResidual = true;

    const double minGapSeconds = (sensitivityControl != nullptr
                                    ? sensitivityControl->getMinGapMs()
                                    : kAutoMinimumGapMs) * 0.001;
    const bool outsideRefractory = (nowSeconds - lastAutoOnsetWallTimeSeconds) >= minGapSeconds;

    if (outsideRefractory && (spectralOnsetDetected || significantLevelRise))
    {
        captureAutoChord (nowSeconds);
        lastAutoOnsetWallTimeSeconds = nowSeconds;
        autoOnsetStrength = 0.0f;
        if (spectralOnsetDetected)
            autoFluxMean = threshold;
    }
}

void AudioPluginAudioProcessorEditor::refreshBassBoostButtonText()
{
    bassBoostButton.setButtonText ("Bass Boost");
}

void AudioPluginAudioProcessorEditor::captureFrozenChord()
{
    const double nowSeconds = juce::Time::getMillisecondCounterHiRes() * 0.001;

    if ((int) frozenChords.size() >= kMaxManualFrozenChords)
    {
        isFrozen = false;
        freezeButton.setToggleState (false, juce::dontSendNotification);
        processorRef.clearLiveFrozenMidiChord();
        showTransientStatusMessage (juce::String (kMaxManualFrozenChords)
                                      + " chords max reached. Press Reset to start a new score.");
        refreshStaffComponent();
        return;
    }

    FrozenChordSnapshot snapshot;
    if (firstFreezeTimeLabel.isEmpty())
        firstFreezeTimeLabel = getCurrentHostTimeLabel();

    snapshot.freqsHz.fill (0.0f);
    const int activePeakCount = getActivePeakCount();
    for (int i = 0; i < activePeakCount; ++i)
        snapshot.freqsHz[(size_t) i] = topFreqsForDrawing[(size_t) i];
    snapshot.partialIntensities = calculatePartialIntensities (
        snapshot.freqsHz,
        topPeakLevelsDbFsForDrawing,
        activePeakCount);

    snapshot.useQuarterToneMode = useQuarterToneMode();
    snapshot.startTimeSeconds = activeFreezeSessionOffsetSeconds
                              + (nowSeconds - activeFreezeSessionStartWallTimeSeconds);
    snapshot.endTimeSeconds = snapshot.startTimeSeconds + 0.25;
    snapshot.visualCreatedWallTimeSeconds = nowSeconds;

    ++freezeCaptureCount;
    snapshot.label = "Freeze " + juce::String (freezeCaptureCount);

    frozenChords.push_back (snapshot);
    processorRef.setLiveFrozenMidiChord (snapshot.freqsHz,
                                         snapshot.partialIntensities,
                                         snapshot.useQuarterToneMode);

    refreshStaffComponent();
}

void AudioPluginAudioProcessorEditor::refreshStaffComponent()
{
    if (staffComponent != nullptr)
        staffComponent->setSnapshots (frozenChords);

    refreshExportButtonState();
}

void AudioPluginAudioProcessorEditor::refreshExportButtonState()
{
    exportMidiButton.setEnabled (! frozenChords.empty());
    exportMidiButton.setButtonText ("Export Pitch");
}

void AudioPluginAudioProcessorEditor::showTransientStatusMessage (juce::String message, double seconds)
{
    transientStatusMessage = std::move (message);
    transientStatusMessageExpirySeconds = juce::Time::getMillisecondCounterHiRes() * 0.001 + seconds;
}

void AudioPluginAudioProcessorEditor::closeActiveFrozenChord()
{
    if (! isFrozen || frozenChords.empty())
        return;

    const double nowSeconds = juce::Time::getMillisecondCounterHiRes() * 0.001;
    auto& snapshot = frozenChords.back();
    snapshot.endTimeSeconds = juce::jmax (snapshot.startTimeSeconds + 0.05,
                                          activeFreezeSessionOffsetSeconds
                                            + (nowSeconds - activeFreezeSessionStartWallTimeSeconds));

    refreshStaffComponent();
}

void AudioPluginAudioProcessorEditor::resetFrozenState()
{
    closeActiveFrozenChord();
    isFrozen = false;
    freezeButton.setToggleState (false, juce::dontSendNotification);
    isAutoRunning = false;
    isDetailPageActive = false;
    hasCompletedAutoAnalysis = false;
    freezeCaptureCount = 0;
    autoCaptureCount = 0;
    activeFreezeSessionStartWallTimeSeconds = 0.0;
    activeFreezeSessionOffsetSeconds = 0.0;
    autoStartWallTimeSeconds = 0.0;
    firstFreezeTimeLabel.clear();
    pendingFreezeMarker = false;
    transientStatusMessage.clear();
    transientStatusMessageExpirySeconds = 0.0;
    frozenChords.clear();
    detailAnalysisHistory.clear();
    processorRef.clearLiveFrozenMidiChord();

    spectrumForDrawing.fill (0.0f);
    topFreqsForDrawing.fill (0.0f);
    topMagsForDrawing.fill (0.0f);
    topPeakLevelsDbFsForDrawing.fill (-120.0f);
    latestSpectrum.fill (0.0f);
    latestResidual.fill (0.0f);
    previousAutoResidual.fill (0.0f);
    latestTopFreqs.fill (0.0f);
    latestTopMags.fill (0.0f);
    latestTopPeakLevelsDbFs.fill (-120.0f);

    clearSpectrogramImage();
    resetAutoDetectorState();

    refreshStaffComponent();
    refreshAutoButtonStates();
    refreshDetailAnalysisButtonState();

    repaint();
}

void AudioPluginAudioProcessorEditor::clearSpectrogramImage()
{
    spectrogramImage.clear (spectrogramImage.getBounds(), juce::Colours::transparentBlack);
}

void AudioPluginAudioProcessorEditor::drawAnalysisCurve (juce::Graphics& g,
                                                         juce::Rectangle<int> area,
                                                         const juce::String& title,
                                                         const juce::String& valueText,
                                                         float minValue,
                                                         float maxValue,
                                                         bool available,
                                                         const std::function<float (const DetailAnalysisFrame&)>& valueForFrame)
{
    auto descriptorArea = area.reduced (2, 0);
    auto header = descriptorArea.removeFromTop (28);
    auto labelArea = header.removeFromLeft ((int) std::round ((float) area.getWidth() * 0.60f));

    g.setFont (makeUIFont (10.8f, false));
    g.setColour (available ? kContrastBlush : kContrastBlush.withAlpha (0.38f));
    g.drawFittedText (title, labelArea, juce::Justification::centredLeft, 1);

    g.setColour (available ? kTextPrimary : kTextSecondary.withAlpha (0.48f));
    g.setFont (makeUIFont (11.8f, false));
    g.drawText (available ? valueText : "Unavailable",
                header,
                juce::Justification::centredRight);

    descriptorArea.removeFromTop (14);
    auto graph = descriptorArea.removeFromTop (juce::jmin (50, descriptorArea.getHeight())).reduced (8, 0);

    g.setColour (juce::Colours::black.withAlpha (0.08f));
    g.drawHorizontalLine (graph.getCentreY(), (float) graph.getX(), (float) graph.getRight());

    if (! available || graph.isEmpty() || detailAnalysisHistory.size() < 2 || maxValue <= minValue)
        return;

    juce::Path curve;
    const auto count = detailAnalysisHistory.size();
    for (size_t i = 0; i < count; ++i)
    {
        const auto value = juce::jlimit (minValue, maxValue, valueForFrame (detailAnalysisHistory[i]));
        const float x = (float) graph.getX()
                      + (float) i * (float) graph.getWidth() / (float) juce::jmax ((size_t) 1, count - 1);
        const float normalised = (value - minValue) / (maxValue - minValue);
        const float y = (float) graph.getBottom() - normalised * (float) graph.getHeight();

        if (i == 0)
            curve.startNewSubPath (x, y);
        else
            curve.lineTo (x, y);
    }

    g.setColour (kButtonTerracottaMuted);
    g.strokePath (curve,
                  juce::PathStrokeType (1.5f,
                                        juce::PathStrokeType::curved,
                                        juce::PathStrokeType::rounded));
}

juce::Rectangle<float> AudioPluginAudioProcessorEditor::getAlternateAnalysisBounds() const
{
    auto bounds = getModuleBounds (getLocalBounds());
    bounds.removeFromRight (kRightPanelWidth);
    bounds.removeFromRight (kMainColumnGap);
    bounds.translate (-kLeftColumnShift, 0);

    bounds.removeFromTop (getExpandedSpectrogramHeight (bounds.getHeight()));
    bounds.removeFromTop (kSpectrogramBottomGap);
    if (! isDetailPageActive)
        bounds = shrinkScoreArea (bounds);

    return bounds.reduced (8, 6).reduced (24, 42).toFloat();
}

juce::Rectangle<float> AudioPluginAudioProcessorEditor::getSpectrogramGraphBounds() const
{
    auto bounds = getModuleBounds (getLocalBounds());
    bounds.removeFromRight (kRightPanelWidth);
    bounds.removeFromRight (kMainColumnGap);
    auto leftPanel = bounds.translated (-kLeftColumnShift, 0);

    auto spectroArea = leftPanel.removeFromTop (getExpandedSpectrogramHeight (leftPanel.getHeight()));
    return spectroArea.reduced (8, 6).reduced (8).toFloat();
}

void AudioPluginAudioProcessorEditor::mouseMove (const juce::MouseEvent& event)
{
    const auto spectrogramGraph = getSpectrogramGraphBounds();
    const bool wasSpectrogramHoverActive = spectrogramHoverActive;
    const float previousHoverFrequencyHz = spectrogramHoverFrequencyHz;

    if (spectrogramGraph.contains (event.position) && processorRef.getSampleRate() > 0.0)
    {
        const float nyquist = (float) processorRef.getSampleRate() * 0.5f;
        const float minFreq = kMinDisplayFreq;
        const float maxFreq = juce::jmin (kMaxDisplayFreq, nyquist);
        const float normalisedY = juce::jlimit (0.0f, 1.0f,
                                                (spectrogramGraph.getBottom() - event.position.y)
                                                    / spectrogramGraph.getHeight());

        spectrogramHoverFrequencyHz = melToHz (hzToMel (minFreq)
                                                + normalisedY * (hzToMel (maxFreq) - hzToMel (minFreq)));
        spectrogramHoverActive = true;
    }
    else
    {
        spectrogramHoverActive = false;
    }

    if (spectrogramHoverActive != wasSpectrogramHoverActive
        || (spectrogramHoverActive
            && std::abs (spectrogramHoverFrequencyHz - previousHoverFrequencyHz) > 1.0f))
        repaint (spectrogramGraph.getSmallestIntegerContainer().expanded (2));

    const auto clearAnalysisHover = [this]() {
        if (analysisHoverTarget >= 0)
        {
            analysisHoverTarget = -1;
            repaint();
        }
    };

    if (! isDetailPageActive || detailViewMode == DetailViewMode::grid || detailAnalysisHistory.size() < 2)
    {
        clearAnalysisHover();
        return;
    }

    const auto content = getAlternateAnalysisBounds();
    const auto mouse = event.position;
    if (! content.contains (mouse))
    {
        clearAnalysisHover();
        return;
    }

    const auto count = detailAnalysisHistory.size();

    if (detailViewMode == DetailViewMode::sparklines)
    {
        const float rowHeight = content.getHeight() / 6.0f;
        const int row = juce::jlimit (0, 5, (int) ((mouse.y - content.getY()) / rowHeight));
        auto graph = juce::Rectangle<float> (content.getX(), content.getY() + rowHeight * (float) row,
                                             content.getWidth(), rowHeight)
                         .reduced (0.0f, 6.0f);
        graph.removeFromLeft (92.0f);
        graph.removeFromRight (72.0f);
        graph = graph.withSizeKeepingCentre (graph.getWidth() * 0.52f, graph.getHeight());

        if (! graph.contains (mouse))
        {
            clearAnalysisHover();
            return;
        }

        const float unitTime = juce::jlimit (0.0f, 1.0f, (mouse.x - graph.getX()) / graph.getWidth());
        const size_t index = juce::jlimit ((size_t)0, count - 1, (size_t) std::round (unitTime * (float) (count - 1)));
        const auto& frame = detailAnalysisHistory[index];
        float normalised = 0.0f;

        switch (row)
        {
        case 0:
        {
            const float hz = juce::jlimit (80.0f, 12000.0f, frame.centroidHz);
            normalised = std::log (hz / 80.0f) / std::log (12000.0f / 80.0f);
            break;
        }
        case 1:
            normalised = (juce::jlimit (-96.0f, 0.0f, frame.rmsDb) + 96.0f) / 96.0f;
            break;
        case 2:
            normalised = juce::jlimit (0.0f, 1.0f, frame.spectralFlux);
            break;
        case 3:
            normalised = juce::jlimit (0.0f, 1.0f, frame.spectralSpreadHz / 12000.0f);
            break;
        case 4:
            normalised = juce::jlimit (0.0f, 1.0f, frame.spectralFlatness);
            break;
        case 5:
            normalised = 0.5f * (juce::jlimit (-1.0f, 1.0f, frame.stereoPanEnergy) + 1.0f);
            break;
        default:
            break;
        }

        analysisHoverTarget = row;
        analysisHoverFrameIndex = index;
        analysisHoverPoint = { graph.getX() + unitTime * graph.getWidth(),
                               graph.getBottom() - normalised * graph.getHeight() };
        repaint();
        return;
    }

    constexpr float panelGap = 24.0f;
    const float panelWidth = (content.getWidth() - panelGap) * 0.5f;
    const juce::Rectangle<float> panels[] = { { content.getX(), content.getY(), panelWidth, content.getHeight() },
                                              { content.getX() + panelWidth + panelGap, content.getY(), panelWidth,
                                                content.getHeight() } };

    float nearestDistance = std::numeric_limits<float>::max();
    int nearestTarget = -1;
    size_t nearestIndex = 0;
    juce::Point<float> nearestPoint;

    for (int panelIndex = 0; panelIndex < 2; ++panelIndex)
    {
        auto graph = panels[panelIndex].reduced (22.0f, 18.0f);
        graph.removeFromTop (28.0f);
        graph.removeFromBottom (18.0f);
        graph.removeFromLeft (16.0f);

        if (! graph.expanded (12.0f).contains (mouse))
            continue;

        const auto maxDrawablePoints = juce::jmax (2, (int) graph.getWidth() * 2);
        const auto step = juce::jmax ((size_t)1, count / (size_t) maxDrawablePoints);

        for (size_t index = 0; index < count; index += step)
        {
            const auto& frame = detailAnalysisHistory[index];
            if (panelIndex == 1 && ! frame.stereoPanAvailable)
                continue;

            const float xValue = panelIndex == 0 ? (juce::jlimit (-96.0f, 0.0f, frame.rmsDb) + 96.0f) / 96.0f
                                                 : juce::jlimit (0.0f, 1.0f, frame.spectralSpreadHz / 12000.0f);
            const float yValue = panelIndex == 0 ? juce::jlimit (0.0f, 1.0f, frame.spectralFlux)
                                                 : 0.5f * (juce::jlimit (-1.0f, 1.0f, frame.stereoPanEnergy) + 1.0f);
            const juce::Point<float> point{ graph.getX() + xValue * graph.getWidth(),
                                            graph.getBottom() - yValue * graph.getHeight() };
            const float distance = mouse.getDistanceFrom (point);
            if (distance < nearestDistance)
            {
                nearestDistance = distance;
                nearestTarget = 6 + panelIndex;
                nearestIndex = index;
                nearestPoint = point;
            }
        }
    }

    if (nearestTarget >= 0 && nearestDistance <= 18.0f)
    {
        analysisHoverTarget = nearestTarget;
        analysisHoverFrameIndex = nearestIndex;
        analysisHoverPoint = nearestPoint;
        repaint();
    }
    else
    {
        clearAnalysisHover();
    }

    return;
}

void AudioPluginAudioProcessorEditor::mouseExit (const juce::MouseEvent&)
{
    const bool hadSpectrogramHover = spectrogramHoverActive;
    spectrogramHoverActive = false;

    if (analysisHoverTarget >= 0)
    {
        analysisHoverTarget = -1;
        repaint();
    }
    else if (hadSpectrogramHover)
    {
        repaint (getSpectrogramGraphBounds().getSmallestIntegerContainer().expanded (2));
    }
}

void AudioPluginAudioProcessorEditor::drawSparklineAnalysisPanel (juce::Graphics& g, juce::Rectangle<int> area)
{
    auto panel = area.reduced (8, 6);
    g.setColour (kPanelUnderlayColour);
    g.fillRoundedRectangle (panel.toFloat().expanded (2.0f), kUnifiedCornerRadius);
    g.setColour (kLargePanelColour);
    g.fillRoundedRectangle (panel.toFloat(), kUnifiedCornerRadius);

    auto header = panel.reduced (14, 10).removeFromTop (kDetailHeaderHeight);
    header.removeFromRight (kDetailDragWidth);
    header.removeFromRight (12);
    header.removeFromRight (kDetailTimeScaleWidth);
    header.removeFromRight (14);

    auto titleArea = header.removeFromLeft (128);
    header.removeFromLeft (12);
    g.setColour (kContrastBlush);
    g.setFont (makeUIFont (11.8f, true));
    g.drawText ("Sparklines", titleArea, juce::Justification::centredLeft);

    g.setColour (kTextSecondary.withAlpha (0.62f));
    g.setFont (makeUIFont (6.6f, false));
    const auto duration = detailAnalysisHistory.empty() ? 0.0 : detailAnalysisHistory.back().timeSeconds;
    g.drawText (juce::String (detailAnalysisHistory.size()) + " frames  /  " + juce::String (duration, 1) + " sec",
                header, juce::Justification::centredLeft);

    if (detailAnalysisHistory.size() < 2)
        return;

    auto content = panel.reduced (24, 42).toFloat();
    g.saveState();
    g.reduceClipRegion (content.toNearestInt());

    const auto highlightColour = kButtonTerracottaMuted.brighter (0.18f);
    const float rowHeight = content.getHeight() / 6.0f;
    const auto count = detailAnalysisHistory.size();
    const bool hasStereoPan = std::any_of (detailAnalysisHistory.begin(), detailAnalysisHistory.end(),
                                           [] (const auto& frame) { return frame.stereoPanAvailable; });

    g.setColour (kDetailDivider);
    g.drawVerticalLine ((int) std::round (content.getX() + 92.0f),
                        content.getY(),
                        content.getBottom());
    g.drawVerticalLine ((int) std::round (content.getRight() - 72.0f),
                        content.getY(),
                        content.getBottom());

    const auto descriptorName = [] (int row) {
        const juce::String names[] = { "CENTROID", "RMS", "FLUX", "SPREAD", "FLATNESS", "STEREO ENERGY" };
        return names[juce::jlimit (0, 5, row)];
    };

    const auto normalisedValue = [] (int row, const DetailAnalysisFrame& frame) {
        switch (row)
        {
        case 0:
        {
            const float hz = juce::jlimit (80.0f, 12000.0f, frame.centroidHz);
            return std::log (hz / 80.0f) / std::log (12000.0f / 80.0f);
        }
        case 1:
            return (juce::jlimit (-96.0f, 0.0f, frame.rmsDb) + 96.0f) / 96.0f;
        case 2:
            return juce::jlimit (0.0f, 1.0f, frame.spectralFlux);
        case 3:
            return juce::jlimit (0.0f, 1.0f, frame.spectralSpreadHz / 12000.0f);
        case 4:
            return juce::jlimit (0.0f, 1.0f, frame.spectralFlatness);
        case 5:
            return 0.5f * (juce::jlimit (-1.0f, 1.0f, frame.stereoPanEnergy) + 1.0f);
        default:
            return 0.0f;
        }
    };

    const auto valueText = [] (int row, const DetailAnalysisFrame& frame, bool stereoAvailable) {
        switch (row)
        {
        case 0:
            return juce::String (frame.centroidHz, 0) + " Hz";
        case 1:
            return frame.rmsDb <= -119.0f ? juce::String ("-inf dB") : juce::String (frame.rmsDb, 1) + " dB";
        case 2:
            return juce::String (frame.spectralFlux, 3);
        case 3:
            return juce::String (frame.spectralSpreadHz, 0) + " Hz";
        case 4:
            return juce::String (frame.spectralFlatness, 3);
        case 5:
            if (! stereoAvailable)
                return juce::String ("Mono");
            if (frame.stereoPanEnergy < -0.04f)
                return juce::String ("L ") + juce::String (std::abs (frame.stereoPanEnergy), 2);
            if (frame.stereoPanEnergy > 0.04f)
                return juce::String ("R ") + juce::String (frame.stereoPanEnergy, 2);
            return juce::String ("Center");
        default:
            return juce::String();
        }
    };

    for (int row = 0; row < 6; ++row)
    {
        auto rowArea = juce::Rectangle<float> (content.getX(), content.getY() + rowHeight * (float) row,
                                               content.getWidth(), rowHeight);
        if (row > 0)
        {
            g.setColour (kDetailDivider);
            g.drawHorizontalLine ((int) rowArea.getY(), content.getX() + 12.0f, content.getRight() - 12.0f);
        }

        auto labelArea = rowArea.removeFromLeft (92.0f).reduced (12.0f, 0.0f);
        auto latestArea = rowArea.removeFromRight (72.0f).reduced (4.0f, 0.0f);
        auto graph = rowArea.reduced (0.0f, 6.0f);
        graph = graph.withSizeKeepingCentre (graph.getWidth() * 0.52f, graph.getHeight());

        g.setFont (makeUIFont (7.2f, true));
        g.setColour (kTextPrimary.withAlpha (0.92f));
        g.drawText (descriptorName (row), labelArea, juce::Justification::centredLeft);

        g.setFont (makeUIFont (7.4f, false));
        g.setColour (kTextPrimary);
        g.drawFittedText (valueText (row, detailAnalysisHistory.back(), hasStereoPan), latestArea.toNearestInt(),
                          juce::Justification::centredRight, 1);

        const bool available = row != 5 || hasStereoPan;
        if (! available)
            continue;

        g.setColour (juce::Colours::black.withAlpha (0.08f));
        g.drawHorizontalLine ((int) std::round (graph.getCentreY()), graph.getX(), graph.getRight());

        juce::Path path;
        const auto maxDrawablePoints = juce::jmax (2, (int) graph.getWidth() * 2);
        const auto step = juce::jmax ((size_t)1, count / (size_t) maxDrawablePoints);
        bool started = false;
        for (size_t index = 0; index < count; index += step)
        {
            const float time = (float) index / (float) (count - 1);
            const float value = normalisedValue (row, detailAnalysisHistory[index]);
            const float x = graph.getX() + time * graph.getWidth();
            const float y = graph.getBottom() - value * graph.getHeight();
            if (! started)
            {
                path.startNewSubPath (x, y);
                started = true;
            }
            else
                path.lineTo (x, y);
        }

        const float lastY =
            graph.getBottom() - normalisedValue (row, detailAnalysisHistory.back()) * graph.getHeight();
        path.lineTo (graph.getRight(), lastY);
        g.setColour (kButtonTerracottaMuted);
        g.strokePath (path, juce::PathStrokeType (1.5f, juce::PathStrokeType::curved, juce::PathStrokeType::rounded));
        if (row != 5)
            g.fillEllipse (juce::Rectangle<float> (3.0f, 3.0f).withCentre ({ graph.getRight(), lastY }));

        if (analysisHoverTarget == row && analysisHoverFrameIndex < detailAnalysisHistory.size())
        {
            g.setColour (highlightColour.withAlpha (0.28f));
            g.drawVerticalLine ((int) std::round (analysisHoverPoint.x), graph.getY(), graph.getBottom());
            if (row != 5)
            {
                g.setColour (highlightColour);
                g.fillEllipse (juce::Rectangle<float> (5.0f, 5.0f).withCentre (analysisHoverPoint));
            }
        }
    }

    if (analysisHoverTarget >= 0 && analysisHoverTarget < 6 && analysisHoverFrameIndex < detailAnalysisHistory.size())
    {
        const auto& frame = detailAnalysisHistory[analysisHoverFrameIndex];
        const auto hoverText = descriptorName (analysisHoverTarget) + "  |  " + juce::String (frame.timeSeconds, 2) +
                               " sec  |  " + valueText (analysisHoverTarget, frame, hasStereoPan);
        const float tooltipWidth = juce::jmin (216.0f, content.getWidth() - 16.0f);
        float tooltipX = analysisHoverPoint.x + 8.0f;
        if (tooltipX + tooltipWidth > content.getRight() - 6.0f)
            tooltipX = analysisHoverPoint.x - tooltipWidth - 8.0f;
        const float tooltipY =
            juce::jlimit (content.getY() + 5.0f, content.getBottom() - 25.0f, analysisHoverPoint.y - 24.0f);
        auto tooltip = juce::Rectangle<float> (tooltipX, tooltipY, tooltipWidth, 20.0f);
        g.setColour (kPanelBackground.withAlpha (0.96f));
        g.fillRoundedRectangle (tooltip, kUnifiedCornerRadius);
        g.setColour (kTextPrimary);
        g.setFont (makeUIFont (7.2f, true));
        g.drawFittedText (hoverText, tooltip.reduced (7.0f, 3.0f).toNearestInt(), juce::Justification::centredLeft, 1);
    }

    g.restoreState();
}

void AudioPluginAudioProcessorEditor::drawRelationalAnalysisPanel (juce::Graphics& g, juce::Rectangle<int> area)
{
    auto panel = area.reduced (8, 6);
    g.setColour (kPanelUnderlayColour);
    g.fillRoundedRectangle (panel.toFloat().expanded (2.0f), kUnifiedCornerRadius);
    g.setColour (kLargePanelColour);
    g.fillRoundedRectangle (panel.toFloat(), kUnifiedCornerRadius);

    auto header = panel.reduced (14, 10).removeFromTop (kDetailHeaderHeight);
    header.removeFromRight (kDetailDragWidth);
    header.removeFromRight (12);
    header.removeFromRight (kDetailTimeScaleWidth);
    header.removeFromRight (14);

    auto titleArea = header.removeFromLeft (128);
    header.removeFromLeft (12);
    g.setColour (kTextPrimary);
    g.setFont (makeUIFont (11.8f, true));
    g.drawText ("Relational View", titleArea, juce::Justification::centredLeft);

    g.setColour (kTextSecondary.withAlpha (0.62f));
    g.setFont (makeUIFont (6.6f, false));
    const auto duration = detailAnalysisHistory.empty() ? 0.0 : detailAnalysisHistory.back().timeSeconds;
    g.drawText (juce::String (detailAnalysisHistory.size()) + " frames  /  " + juce::String (duration, 1) + " sec",
                header, juce::Justification::centredLeft);

    if (detailAnalysisHistory.size() < 2)
        return;

    auto content = panel.reduced (24, 42).toFloat();
    constexpr float panelGap = 24.0f;
    const float panelWidth = (content.getWidth() - panelGap) * 0.5f;
    const juce::Rectangle<float> relationPanels[] = {
        { content.getX(), content.getY(), panelWidth, content.getHeight() },
        { content.getX() + panelWidth + panelGap, content.getY(), panelWidth, content.getHeight() }
    };
    const auto traceColour = kSpectrogramLabel.withAlpha (0.72f);
    const auto accentColour = kButtonTerracottaMuted.brighter (0.18f);
    const auto count = detailAnalysisHistory.size();
    const bool hasStereoPan = std::any_of (detailAnalysisHistory.begin(), detailAnalysisHistory.end(),
                                           [] (const auto& frame) { return frame.stereoPanAvailable; });

    g.setColour (kDetailDivider);
    g.drawVerticalLine ((int) std::round (content.getCentreX()),
                        content.getY(),
                        content.getBottom());

    for (int panelIndex = 0; panelIndex < 2; ++panelIndex)
    {
        const auto relationPanel = relationPanels[panelIndex];

        auto title = relationPanel.reduced (14.0f, 8.0f).removeFromTop (18.0f);
        g.setFont (makeUIFont (8.2f, true));
        g.setColour (kTextPrimary);
        g.drawText (panelIndex == 0 ? "RMS x SPECTRAL FLUX" : "SPREAD x STEREO ENERGY", title,
                    juce::Justification::centredLeft);

        auto graph = relationPanel.reduced (22.0f, 18.0f);
        graph.removeFromTop (28.0f);
        graph.removeFromBottom (18.0f);
        graph.removeFromLeft (16.0f);

        g.setColour (kSpectrogramLabel.withAlpha (0.10f));
        g.drawHorizontalLine ((int) std::round (graph.getCentreY()), graph.getX(), graph.getRight());
        g.drawVerticalLine ((int) std::round (graph.getCentreX()), graph.getY(), graph.getBottom());
        g.setColour (kSpectrogramLabel.withAlpha (0.25f));
        g.drawLine (graph.getX(), graph.getBottom(), graph.getRight(), graph.getBottom(), 0.8f);
        g.drawLine (graph.getX(), graph.getY(), graph.getX(), graph.getBottom(), 0.8f);

        g.setFont (makeUIFont (6.7f, false));
        g.setColour (kTextPrimary.withAlpha (0.76f));
        g.drawText (panelIndex == 0 ? "quiet" : "narrow",
                    juce::Rectangle<float> (graph.getX(), graph.getBottom() + 2.0f, 46.0f, 12.0f),
                    juce::Justification::centredLeft);
        g.drawText (panelIndex == 0 ? "loud" : "wide",
                    juce::Rectangle<float> (graph.getRight() - 46.0f, graph.getBottom() + 2.0f, 46.0f, 12.0f),
                    juce::Justification::centredRight);
        g.drawText (panelIndex == 0 ? "active" : "R",
                    juce::Rectangle<float> (relationPanel.getX() + 3.0f, graph.getY() - 5.0f, 34.0f, 12.0f),
                    juce::Justification::centred);
        g.drawText (panelIndex == 0 ? "stable" : "L",
                    juce::Rectangle<float> (relationPanel.getX() + 3.0f, graph.getBottom() - 7.0f, 34.0f, 12.0f),
                    juce::Justification::centred);

        if (panelIndex == 1 && ! hasStereoPan)
        {
            g.setColour (kTextPrimary.withAlpha (0.78f));
            g.setFont (makeUIFont (8.0f, false));
            g.drawText ("Mono input - stereo trajectory unavailable", graph.toNearestInt(),
                        juce::Justification::centred);
            continue;
        }

        juce::Path trajectory;
        bool started = false;
        juce::Point<float> startPoint;
        juce::Point<float> endPoint;
        const auto maxDrawablePoints = juce::jmax (2, (int) graph.getWidth() * 2);
        const auto step = juce::jmax ((size_t)1, count / (size_t) maxDrawablePoints);

        const auto pointForFrame = [panelIndex, graph] (const DetailAnalysisFrame& frame) {
            const float xValue = panelIndex == 0 ? (juce::jlimit (-96.0f, 0.0f, frame.rmsDb) + 96.0f) / 96.0f
                                                 : juce::jlimit (0.0f, 1.0f, frame.spectralSpreadHz / 12000.0f);
            const float yValue = panelIndex == 0 ? juce::jlimit (0.0f, 1.0f, frame.spectralFlux)
                                                 : 0.5f * (juce::jlimit (-1.0f, 1.0f, frame.stereoPanEnergy) + 1.0f);
            return juce::Point<float> (graph.getX() + xValue * graph.getWidth(),
                                       graph.getBottom() - yValue * graph.getHeight());
        };

        for (size_t index = 0; index < count; index += step)
        {
            const auto& frame = detailAnalysisHistory[index];
            if (panelIndex == 1 && ! frame.stereoPanAvailable)
                continue;

            const auto point = pointForFrame (frame);
            if (! started)
            {
                trajectory.startNewSubPath (point);
                startPoint = point;
                started = true;
            }
            else
                trajectory.lineTo (point);
            endPoint = point;
        }

        const auto& lastFrame = detailAnalysisHistory.back();
        if (panelIndex == 0 || lastFrame.stereoPanAvailable)
        {
            endPoint = pointForFrame (lastFrame);
            trajectory.lineTo (endPoint);
        }

        g.setColour (traceColour);
        g.strokePath (trajectory,
                      juce::PathStrokeType (1.2f, juce::PathStrokeType::curved, juce::PathStrokeType::rounded));
        g.setColour (kSpectrogramLabel.withAlpha (0.60f));
        g.drawEllipse (juce::Rectangle<float> (5.0f, 5.0f).withCentre (startPoint), 0.9f);
        g.setColour (accentColour);
        g.fillEllipse (juce::Rectangle<float> (5.0f, 5.0f).withCentre (endPoint));

        g.setFont (makeUIFont (6.4f, false));
        g.setColour (kTextPrimary.withAlpha (0.72f));
        g.drawText ("start", juce::Rectangle<float> (startPoint.x + 4.0f, startPoint.y - 8.0f, 32.0f, 12.0f),
                    juce::Justification::centredLeft);
        g.drawText ("now", juce::Rectangle<float> (endPoint.x + 4.0f, endPoint.y - 8.0f, 28.0f, 12.0f),
                    juce::Justification::centredLeft);

        if (analysisHoverTarget == 6 + panelIndex && analysisHoverFrameIndex < detailAnalysisHistory.size())
        {
            g.setColour (accentColour.withAlpha (0.18f));
            g.fillEllipse (juce::Rectangle<float> (13.0f, 13.0f).withCentre (analysisHoverPoint));
            g.setColour (accentColour);
            g.fillEllipse (juce::Rectangle<float> (4.5f, 4.5f).withCentre (analysisHoverPoint));
        }
    }

    if (analysisHoverTarget >= 6 && analysisHoverTarget <= 7 && analysisHoverFrameIndex < detailAnalysisHistory.size())
    {
        const auto& frame = detailAnalysisHistory[analysisHoverFrameIndex];
        const bool isRmsFlux = analysisHoverTarget == 6;
        const auto hoverText = juce::String (frame.timeSeconds, 2) + " sec  |  " +
                               (isRmsFlux ? juce::String ("RMS ") + juce::String (frame.rmsDb, 1) + " dB  |  Flux " +
                                                juce::String (frame.spectralFlux, 3)
                                          : juce::String ("Spread ") + juce::String (frame.spectralSpreadHz, 0) +
                                                " Hz  |  Stereo " + juce::String (frame.stereoPanEnergy, 2));
        const float tooltipWidth = juce::jmin (230.0f, content.getWidth() * 0.5f - 14.0f);
        const auto ownerPanel = relationPanels[analysisHoverTarget - 6];
        float tooltipX = analysisHoverPoint.x + 8.0f;
        if (tooltipX + tooltipWidth > ownerPanel.getRight() - 5.0f)
            tooltipX = analysisHoverPoint.x - tooltipWidth - 8.0f;
        const float tooltipY =
            juce::jlimit (ownerPanel.getY() + 30.0f, ownerPanel.getBottom() - 25.0f, analysisHoverPoint.y - 24.0f);
        auto tooltip = juce::Rectangle<float> (tooltipX, tooltipY, tooltipWidth, 20.0f);
        g.setColour (kPanelBackground.withAlpha (0.96f));
        g.fillRoundedRectangle (tooltip, kUnifiedCornerRadius);
        g.setColour (kTextPrimary);
        g.setFont (makeUIFont (7.0f, true));
        g.drawFittedText (hoverText, tooltip.reduced (7.0f, 3.0f).toNearestInt(), juce::Justification::centredLeft, 1);
    }
}

void AudioPluginAudioProcessorEditor::drawDetailAnalysisPanel (juce::Graphics& g, juce::Rectangle<int> area)
{
    auto panel = area.reduced (8, 6);
    g.setColour (kPanelUnderlayColour);
    g.fillRoundedRectangle (panel.toFloat().expanded (2.0f), kUnifiedCornerRadius);
    g.setColour (kLargePanelColour);
    g.fillRoundedRectangle (panel.toFloat(), kUnifiedCornerRadius);

    auto header = panel.reduced (14, 10).removeFromTop (kDetailHeaderHeight);
    header.removeFromRight (kDetailDragWidth);
    header.removeFromRight (12);
    header.removeFromRight (kDetailTimeScaleWidth);
    header.removeFromRight (14);

    auto titleArea = header.removeFromLeft (128);
    header.removeFromLeft (12);
    auto summaryArea = header;

    g.setColour (kContrastBlush);
    g.setFont (makeUIFont (11.8f, true));
    g.drawText ("Detail Analysis", titleArea, juce::Justification::centredLeft);

    g.setColour (kTextSecondary.withAlpha (0.62f));
    g.setFont (makeUIFont (6.6f, false));
    const auto duration = detailAnalysisHistory.empty() ? 0.0 : detailAnalysisHistory.back().timeSeconds;
    g.drawText (juce::String (detailAnalysisHistory.size()) + " frames  /  "
                + juce::String (duration, 1) + " sec",
                summaryArea,
                juce::Justification::centredLeft);

    if (detailAnalysisHistory.empty())
    {
        g.setColour (kContrastBlush.withAlpha (0.74f));
        g.setFont (makeUIFont (10.0f, false));
        g.drawText ("Run Auto Mode, then Stop to unlock analysis.",
                    panel,
                    juce::Justification::centred);
        return;
    }

    const auto& latest = detailAnalysisHistory.back();
    const bool hasStereoPan = std::any_of (detailAnalysisHistory.begin(),
                                           detailAnalysisHistory.end(),
                                           [] (const auto& frame) { return frame.stereoPanAvailable; });

    auto content = panel.reduced (24, 0);
    content.removeFromTop (44);
    content.removeFromBottom (40);

    constexpr int columnGap = 24;
    constexpr int rowGap = 20;
    const int rowHeight = (content.getHeight() - rowGap) / 2;
    auto topRow = content.removeFromTop (rowHeight);
    content.removeFromTop (rowGap);
    auto bottomRow = content;

    const auto splitIntoThreeColumns = [] (juce::Rectangle<int> row)
    {
        const int columnWidth = (row.getWidth() - columnGap * 2) / 3;
        std::array<juce::Rectangle<int>, 3> columns;
        columns[0] = row.removeFromLeft (columnWidth);
        row.removeFromLeft (columnGap);
        columns[1] = row.removeFromLeft (columnWidth);
        row.removeFromLeft (columnGap);
        columns[2] = row;
        return columns;
    };

    const auto topColumns = splitIntoThreeColumns (topRow);
    const auto bottomColumns = splitIntoThreeColumns (bottomRow);

    const int firstDividerX = (topColumns[0].getRight() + topColumns[1].getX()) / 2;
    const int secondDividerX = (topColumns[1].getRight() + topColumns[2].getX()) / 2;
    const int middleDividerY = topRow.getBottom() + rowGap / 2;

    g.setColour (kDetailDivider);
    g.drawVerticalLine (firstDividerX, (float) topRow.getY(), (float) bottomRow.getBottom());
    g.drawVerticalLine (secondDividerX, (float) topRow.getY(), (float) bottomRow.getBottom());
    g.drawHorizontalLine (middleDividerY,
                          (float) content.getX(),
                          (float) content.getRight());

    const auto flatnessArea = topColumns[0];
    const auto spreadArea = topColumns[1];
    const auto fluxArea = topColumns[2];
    const auto centroidArea = bottomColumns[0];
    const auto rmsArea = bottomColumns[1];
    const auto panArea = bottomColumns[2];

    drawAnalysisCurve (g,
                       flatnessArea,
                       "Spectral Flatness",
                       juce::String (latest.spectralFlatness, 3),
                       0.0f,
                       1.0f,
                       true,
                       [] (const auto& frame) { return frame.spectralFlatness; });

    drawAnalysisCurve (g,
                       spreadArea,
                       "Spectral Spread",
                       juce::String (latest.spectralSpreadHz, 0) + " Hz",
                       0.0f,
                       12000.0f,
                       true,
                       [] (const auto& frame) { return frame.spectralSpreadHz; });

    drawAnalysisCurve (g,
                       fluxArea,
                       "Spectral Flux",
                       juce::String (latest.spectralFlux, 3),
                       0.0f,
                       1.0f,
                       true,
                       [] (const auto& frame) { return frame.spectralFlux; });

    drawAnalysisCurve (g,
                       centroidArea,
                       "Centroid",
                       juce::String (latest.centroidHz, 0) + " Hz",
                       0.0f,
                       24000.0f,
                       true,
                       [] (const auto& frame) { return frame.centroidHz; });

    drawAnalysisCurve (g,
                       rmsArea,
                       "RMS Level",
                       latest.rmsDb <= -119.0f ? juce::String ("-inf dB")
                                               : juce::String (latest.rmsDb, 1) + " dB",
                       -96.0f,
                       6.0f,
                       true,
                       [] (const auto& frame) { return frame.rmsDb; });

    drawAnalysisCurve (g,
                       panArea,
                       "Stereo Pan Energy",
                       latest.stereoPanEnergy < -0.04f ? juce::String ("L ") + juce::String (std::abs (latest.stereoPanEnergy), 2)
                         : latest.stereoPanEnergy > 0.04f ? juce::String ("R ") + juce::String (latest.stereoPanEnergy, 2)
                                                          : juce::String ("Center"),
                       -1.0f,
                       1.0f,
                       hasStereoPan,
                       [] (const auto& frame) { return frame.stereoPanEnergy; });

}

//==============================================================================
void AudioPluginAudioProcessorEditor::paint (juce::Graphics& g)
{
    g.fillAll (kEditorBackground);
    drawImageCover (g, getEditorBackgroundImage(), getLocalBounds().toFloat());
    g.fillAll (kBackgroundVeil);

    auto fullBounds = getLocalBounds();
    auto bounds = getModuleBounds (fullBounds);
    auto rightPanel = bounds.removeFromRight (kRightPanelWidth).translated (kRightColumnShift, 0);
    bounds.removeFromRight (kMainColumnGap);
    auto leftPanel = bounds.translated (-kLeftColumnShift, 0);

    auto spectroArea = leftPanel.removeFromTop (getExpandedSpectrogramHeight (leftPanel.getHeight()));
    leftPanel.removeFromTop (kSpectrogramBottomGap);
    auto scoreArea = isDetailPageActive ? leftPanel : shrinkScoreArea (leftPanel);

    g.setColour (kSidebarUnderlayColour);
    g.fillRoundedRectangle (rightPanel.toFloat().expanded (2.0f), kUnifiedCornerRadius);
    g.setColour (kSidebarPanelColour);
    g.fillRoundedRectangle (rightPanel.toFloat(), kUnifiedCornerRadius);

    if (! isDetailPageActive)
    {
        const auto scorePanel = scoreArea.reduced (8, 6).toFloat();
        g.setColour (kPanelUnderlayColour);
        g.fillRoundedRectangle (scorePanel.expanded (2.0f), kUnifiedCornerRadius);
        g.setColour (kLargePanelColour);
        g.fillRoundedRectangle (scorePanel, kUnifiedCornerRadius);
    }

    auto titleArea = rightPanel.removeFromTop (kTitleAreaHeight);
    const auto titleFont = makeTitleFont (26.0f);
    drawTrackedText (g,
                     "SPEKANA",
                     titleArea.reduced (8, 0).toFloat(),
                     titleFont,
                     kTextPrimary.withAlpha (0.90f),
                     titleFont.getHeight() * 0.02f);

    const double nowSeconds = juce::Time::getMillisecondCounterHiRes() * 0.001;

    if (transientStatusMessage.isNotEmpty() && nowSeconds >= transientStatusMessageExpirySeconds)
        {
            transientStatusMessage.clear();
        }

    auto rightInner = rightPanel.reduced (kRightPanelInset);
    auto bottomInfoArea = rightInner.removeFromBottom (kBottomInfoHeight);
    auto textArea = rightInner.removeFromBottom (kPeakTextAreaHeight);

    if (tuningSelector != nullptr)
    {
        const float separatorY = (float) tuningSelector->getY() - 7.0f;
        g.setColour (kTextPrimary.withAlpha (0.10f));
        g.drawHorizontalLine ((int) std::round (separatorY),
                              (float) rightInner.getX() + 4.0f,
                              (float) rightInner.getRight() - 4.0f);
    }

    auto plotArea = spectroArea.reduced (8, 6);

    g.setColour (kPanelUnderlayColour);
    g.fillRoundedRectangle (plotArea.toFloat().expanded (2.0f), kUnifiedCornerRadius);
    g.setColour (kLargePanelColour);
    g.fillRoundedRectangle (plotArea.toFloat(), kUnifiedCornerRadius);

    const auto spectrogramCanvas = plotArea.reduced (8);
    g.setColour (kSpectrogramCanvasBackground);
    g.fillRoundedRectangle (spectrogramCanvas.toFloat(), kUnifiedCornerRadius);

    g.setOpacity (1.0f);
    g.drawImageWithin (spectrogramImage,
                       (int) plotArea.getX() + 8,
                       (int) plotArea.getY() + 8,
                       (int) plotArea.getWidth() - 16,
                       (int) plotArea.getHeight() - 16,
                       juce::RectanglePlacement::stretchToFit);

    {
        const float sampleRate = (float) processorRef.getSampleRate();
        const float nyquist    = sampleRate * 0.5f;

        const float minFreq = kMinDisplayFreq;
        const float maxFreq = juce::jmin (kMaxDisplayFreq, nyquist);

        const float melMin = hzToMel (minFreq);
        const float melMax = hzToMel (maxFreq);

        const float tickFreqs[] = { 100.0f, 300.0f, 800.0f, 2000.0f, 5000.0f };

        const auto graphBounds = plotArea.reduced (8);
        g.setColour (kSpectrogramLine);
        g.setFont (makeUIFont (7.0f, false));

        for (float f : tickFreqs)
        {
            if (f < minFreq || f > maxFreq || f > nyquist)
                continue;

            float mel = hzToMel (f);

            // 计算在 Mel 范围里的归一化位置
            float norm = (mel - melMin) / (melMax - melMin);
            norm = juce::jlimit (0.0f, 1.0f, norm);

            // 对应到 plotArea 里的 y 坐标（注意下方是低频）
            float y = graphBounds.getBottom() - norm * graphBounds.getHeight();

            g.drawLine ((float) graphBounds.getX(), y, (float) graphBounds.getRight(), y, 0.8f);

            juce::Rectangle<float> labelArea {
                graphBounds.getX() + 4.0f,
                y - 7.0f,
                60.0f,
                14.0f
            };

            juce::String label;
            if (f >= 1000.0f)
                label << juce::String (f / 1000.0f, 1) << " kHz";
            else
                label << juce::String ((int) f) << " Hz";

            g.setColour (kSpectrogramStaticLabel);
            g.drawFittedText (label, labelArea.toNearestInt(),
                              juce::Justification::centredLeft, 1);
            g.setColour (kSpectrogramLine);
        }

        if (spectrogramHoverActive && spectrogramHoverFrequencyHz >= minFreq
            && spectrogramHoverFrequencyHz <= maxFreq)
        {
            const float hoverMel = hzToMel (spectrogramHoverFrequencyHz);
            const float hoverNorm = juce::jlimit (0.0f, 1.0f, (hoverMel - melMin) / (melMax - melMin));
            const float hoverY = graphBounds.getBottom() - hoverNorm * graphBounds.getHeight();

            g.setColour (kSpectrogramLabel.withAlpha (0.58f));
            g.drawLine ((float) graphBounds.getX(), hoverY,
                        (float) graphBounds.getRight(), hoverY, 1.1f);

            juce::String hoverLabel;
            if (spectrogramHoverFrequencyHz >= 1000.0f)
                hoverLabel << juce::String (spectrogramHoverFrequencyHz / 1000.0f, 2) << " kHz";
            else
                hoverLabel << juce::String ((int) std::round (spectrogramHoverFrequencyHz)) << " Hz";

            auto hoverLabelArea = juce::Rectangle<float> (graphBounds.getX() + 4.0f,
                                                          hoverY - 7.0f,
                                                          64.0f,
                                                          14.0f);
            g.setColour (kAmberText);
            g.drawFittedText (hoverLabel, hoverLabelArea.toNearestInt(),
                              juce::Justification::centredLeft, 1);
        }
    }

    if (transientStatusMessage.isNotEmpty())
    {
        auto messageArea = plotArea.reduced (18, 12).removeFromBottom (24);
        g.setColour (kButtonTerracotta.withAlpha (0.68f));
        g.fillRoundedRectangle (messageArea.toFloat(), kUnifiedCornerRadius);
        g.setColour (kAmberText);
        g.setFont (makeUIFont (8.2f, false));
        g.drawFittedText (transientStatusMessage,
                          messageArea.reduced (8, 4),
                          juce::Justification::centredLeft,
                          2);
    }

    if (isDetailPageActive)
    {
        switch (detailViewMode)
        {
            case DetailViewMode::grid:        drawDetailAnalysisPanel (g, scoreArea); break;
            case DetailViewMode::sparklines:  drawSparklineAnalysisPanel (g, scoreArea); break;
            case DetailViewMode::relational:  drawRelationalAnalysisPanel (g, scoreArea); break;
        }

    }

    auto inner = textArea.reduced (2, 4);

    g.setColour (kTextPrimary);
    g.setFont (makeUIFont (12.2f, true));
    auto peaksTitleArea = inner.removeFromTop (24);
    const int activePeakCount = getActivePeakCount();
    g.drawText ("Top " + juce::String (activePeakCount) + " Peaks",
                peaksTitleArea,
                juce::Justification::centredLeft);

    inner.removeFromTop (2);
    auto peakCountControlArea = inner.removeFromTop (24);
    g.setColour (kTextSecondary);
    g.setFont (makeUIFont (9.4f, false));
    g.drawText ("Count",
                peakCountControlArea.removeFromLeft (36),
                juce::Justification::centredLeft);
    inner.removeFromTop (4);

    g.setFont (makeUIFont (9.8f, false));

    const int numCols       = 2;
    const int rowsPerColumn = juce::jmax (1, (activePeakCount + numCols - 1) / numCols);
    const int colWidth   = inner.getWidth()  / numCols;
    const int lineHeight = inner.getHeight() / rowsPerColumn;

    g.setColour (kTextPrimary.withAlpha (0.10f));
    g.drawVerticalLine (inner.getCentreX(), (float) inner.getY() + 3.0f, (float) inner.getBottom() - 3.0f);

    for (int i = 0; i < activePeakCount; ++i)
    {
        int col = i / rowsPerColumn;
        int row = i % rowsPerColumn;

        juce::Rectangle<int> cell;
        cell.setX (inner.getX() + col * colWidth);
        cell.setY (inner.getY() + row * lineHeight);
        cell.setWidth  (colWidth);
        cell.setHeight (lineHeight);

        auto cellContent = cell.reduced (3, 1);
        auto indexArea = cellContent.removeFromLeft (15);
        auto noteArea = cellContent.removeFromLeft (25);
        auto frequencyArea = cellContent;

        g.setColour (kTextSecondary.withAlpha (0.66f));
        g.setFont (makeUIFont (8.7f, false));
        g.drawText (juce::String (i + 1), indexArea, juce::Justification::centredLeft);

        const float freq = topFreqsForDrawing[(size_t) i];

        if (freq > 0.0f)
        {
            g.setColour (kTextPrimary);
            g.setFont (makeUIFont (9.8f, true));
            g.drawFittedText (freqToPitchName (freq, useQuarterToneMode()),
                              noteArea,
                              juce::Justification::centredLeft,
                              1);

            g.setColour (kTextSecondary.withAlpha (0.86f));
            g.setFont (makeUIFont (8.5f, false));
            g.drawFittedText (juce::String (freq, 1) + " Hz",
                              frequencyArea,
                              juce::Justification::centredRight,
                              1);
        }
        else
        {
            g.setColour (kTextSecondary.withAlpha (0.44f));
            g.setFont (makeUIFont (9.2f, false));
            g.drawText ("-", noteArea.getUnion (frequencyArea), juce::Justification::centredLeft);
        }
    }

    if (isAutoMode())
    {
        auto autoReadoutArea = captureModeSelector != nullptr
                                 ? captureModeSelector->getBounds()
                                       .withY (autoClearButton.getBottom() + 2)
                                       .withHeight (14)
                                 : autoClearButton.getBounds().expanded (48, 0)
                                       .withY (autoClearButton.getBottom() + 2)
                                       .withHeight (14);
        auto stateArea = autoReadoutArea;
        auto captureArea = stateArea.removeFromRight (66);
        auto dotArea = stateArea.removeFromLeft (12);
        g.setColour ((isAutoRunning ? kButtonTerracottaMuted : kTextSecondary).withAlpha (0.92f));
        g.fillEllipse ((float) dotArea.getCentreX() - 2.5f,
                       (float) dotArea.getCentreY() - 2.5f,
                       5.0f,
                       5.0f);
        g.setColour (kTextSecondary.withAlpha (0.90f));
        g.setFont (makeUIFont (8.2f, false));
        g.drawText (isAutoRunning ? "Capturing" : "Auto stopped",
                    stateArea,
                    juce::Justification::centredLeft);
        g.drawText (juce::String (autoCaptureCount) + " captures",
                    captureArea,
                    juce::Justification::centredRight);
    }

    auto statusBar = bottomInfoArea.reduced (0, 4);
    g.setColour (kPanelTint.withAlpha (0.58f));
    g.fillPath (makeOrganicCapsulePath (statusBar.toFloat(), 0.18f));

    const auto inputLevelDb = processorRef.getInputLevelDb();
    const juce::String inputText = inputLevelDb <= -119.0f
                                     ? juce::String ("-inf")
                                     : juce::String (inputLevelDb, 0) + "dB";
    const auto statusText = juce::String ("LIVE");

    auto liveArea = statusBar.removeFromLeft (47);
    auto dotArea = liveArea.removeFromLeft (13);
    g.setColour ((isAutoRunning || isFrozen ? kButtonTerracottaMuted : kTextSecondary).withAlpha (0.92f));
    g.fillEllipse ((float) dotArea.getCentreX() - 2.7f,
                   (float) dotArea.getCentreY() - 2.7f,
                   5.4f,
                   5.4f);
    g.setColour (kTextPrimary);
    g.setFont (makeUIFont (8.1f, true));
    g.drawFittedText (statusText, liveArea, juce::Justification::centredLeft, 1);

    const juce::String compactStatus[] { inputText,
                                         useQuarterToneMode() ? "QUARTER" : "SEMI",
                                         bassBoostButton.getToggleState() ? "BASS+" : "BASS-" };
    const int statusWidths[] { 43, 44, statusBar.getWidth() - 87 };
    g.setFont (makeUIFont (7.8f, false));
    for (int index = 0; index < 3; ++index)
    {
        auto segment = statusBar.removeFromLeft (juce::jmax (1, statusWidths[index]));
        g.setColour (index == 2 && bassBoostButton.getToggleState()
                        ? kAmberText
                        : kTextSecondary.withAlpha (0.90f));
        g.drawFittedText (compactStatus[index], segment.reduced (2, 0), juce::Justification::centred, 1);
    }
}


void AudioPluginAudioProcessorEditor::resized()
{
    auto full = getLocalBounds();
    auto bounds = getModuleBounds (full);

    auto rightPanel = bounds.removeFromRight (kRightPanelWidth).translated (kRightColumnShift, 0);
    bounds.removeFromRight (kMainColumnGap);
    auto leftPanel = bounds.translated (-kLeftColumnShift, 0);

    auto spectroArea = leftPanel.removeFromTop (getExpandedSpectrogramHeight (leftPanel.getHeight()));
    leftPanel.removeFromTop (kSpectrogramBottomGap);
    auto scoreArea = isDetailPageActive ? leftPanel : shrinkScoreArea (leftPanel);

    auto graphArea = spectroArea.reduced (16, 14);
    const int w = juce::jmax (1, graphArea.getWidth());
    const int h = juce::jmax (1, graphArea.getHeight());
    spectrogramImage = juce::Image (juce::Image::ARGB, w, h, true);
    clearSpectrogramImage();

    if (staffComponent != nullptr)
    {
        staffComponent->setBounds (scoreArea);
        staffComponent->setVisible (! isDetailPageActive);
    }

    if (descriptorMidiDragComponent != nullptr)
    {
        auto detailHeader = scoreArea.reduced (8, 6).reduced (14, 10).removeFromTop (kDetailHeaderHeight);
        const auto dragMidiBounds = detailHeader.removeFromRight (kDetailDragWidth);
        detailHeader.removeFromRight (12);
        const auto timeScaleBounds = detailHeader.removeFromRight (kDetailTimeScaleWidth);
        const bool showDetailControls = isDetailPageActive
                                     && hasCompletedAutoAnalysis
                                     && ! detailAnalysisHistory.empty()
                                     && ! isAutoRunning;

        descriptorMidiDragComponent->setBounds (dragMidiBounds);
        descriptorMidiDragComponent->setVisible (showDetailControls);
        descriptorMidiDragComponent->toFront (false);

        if (midiTimeScaleSelector != nullptr)
        {
            midiTimeScaleSelector->setBounds (timeScaleBounds);
            midiTimeScaleSelector->setVisible (showDetailControls);
            midiTimeScaleSelector->toFront (false);
        }
    }

    rightPanel.removeFromTop (kTitleAreaHeight);
    auto rightInner = rightPanel.reduced (kRightPanelInset);
    rightInner.removeFromTop (6);
    rightInner.removeFromBottom (kBottomInfoHeight);
    rightInner.removeFromBottom (kPeakTextAreaHeight);
    rightInner.removeFromBottom (8);

    auto controlArea = rightInner;
    if (captureModeSelector != nullptr)
        captureModeSelector->setBounds (controlArea.removeFromTop (32));
    controlArea.removeFromTop (8);

    auto placeSecondaryPair = [&controlArea] (juce::TextButton& left,
                                              juce::TextButton& right)
    {
        auto row = controlArea.removeFromTop (22);
        constexpr int gap = 8;
        const int buttonWidth = (row.getWidth() - gap) / 2;
        left.setBounds (row.removeFromLeft (buttonWidth));
        row.removeFromLeft (gap);
        right.setBounds (row);
    };

    if (isAutoMode())
    {
        auto primaryRow = controlArea.removeFromTop (42);
        autoStartButton.setBounds (primaryRow.withSizeKeepingCentre (94, 40));
        controlArea.removeFromTop (4);
        auto clearRow = controlArea.removeFromTop (20);
        autoClearButton.setBounds (clearRow.withSizeKeepingCentre (68, 20));
        controlArea.removeFromTop (25); // status readout is painted in this gap

        if (sensitivityControl != nullptr)
            sensitivityControl->setBounds (controlArea.removeFromTop (45));
        controlArea.removeFromTop (8);
    }
    else
    {
        auto primaryRow = controlArea.removeFromTop (42);
        freezeButton.setBounds (primaryRow.withSizeKeepingCentre (94, 40));
        controlArea.removeFromTop (4);
        placeSecondaryPair (unfreezeButton, resetButton);
        controlArea.removeFromTop (13);
    }

    if (tuningSelector != nullptr)
        tuningSelector->setBounds (controlArea.removeFromTop (30));
    controlArea.removeFromTop (6);
    exportMidiButton.setBounds (controlArea.removeFromTop (23));

    auto peakPanel = getModuleBounds (getLocalBounds()).removeFromRight (kRightPanelWidth)
                                                     .translated (kRightColumnShift, 0)
                                                     .reduced (kRightPanelInset);
    peakPanel.removeFromTop (kTitleAreaHeight);
    peakPanel.removeFromBottom (kBottomInfoHeight);
    auto peakTextArea = peakPanel.removeFromBottom (kPeakTextAreaHeight);
    auto peakInner = peakTextArea.reduced (2, 4);
    peakInner.removeFromTop (26);
    topPeakCountSlider.setBounds (peakInner.removeFromTop (24).withTrimmedLeft (37).reduced (0, 2));

    const bool showDetailViews = isDetailPageActive
                              && hasCompletedAutoAnalysis
                              && ! detailAnalysisHistory.empty()
                              && ! isAutoRunning;

    constexpr int footerControlHeight = 40;
    const int lowerControlY = scoreArea.getBottom() - 47;
    if (showDetailViews)
    {
        detailAnalysisButton.setBounds (scoreArea.getX() + 19, lowerControlY, 190, footerControlHeight);
        bassBoostButton.setVisible (false);

        if (detailViewSelector != nullptr)
        {
            const int selectorX = detailAnalysisButton.getRight() + 8;
            const int selectorWidth = juce::jmax (180, scoreArea.getRight() - 19 - selectorX);
            detailViewSelector->setBounds (selectorX, lowerControlY, selectorWidth, footerControlHeight);
            detailViewSelector->setVisible (true);
            detailViewSelector->toFront (false);
        }
    }
    else
    {
        bassBoostButton.setVisible (true);
        bassBoostButton.setBounds (scoreArea.getX() + 19, lowerControlY, 150, footerControlHeight);
        detailAnalysisButton.setBounds (bassBoostButton.getRight() + 12, lowerControlY, 190, footerControlHeight);

        if (detailViewSelector != nullptr)
            detailViewSelector->setVisible (false);
    }

    bassBoostButton.toFront (false);
    detailAnalysisButton.toFront (false);
}


void AudioPluginAudioProcessorEditor::pushSpectrumToImage()
{
    auto width   = spectrogramImage.getWidth();
    auto height  = spectrogramImage.getHeight();
    auto numBins = (int) spectrumForDrawing.size();

    if (width <= 0 || height <= 0 || numBins <= 0)
        return;

    // 1. 时间轴向右滚：把整张图往左移 1 像素
    spectrogramImage.moveImageSection(
        0, 0,           // dst x, y
        1, 0,           // src x, y（从第二列开始）
        width - 1,      // 宽度：去掉最右一列
        height
    );

    const float sampleRate = (float) processorRef.getSampleRate();
    if (sampleRate <= 0.0f)
        return;

    const float nyquist = sampleRate * 0.5f;

    // ===== 频率范围 & mel 映射参数（这组只定义一次）=====
    const float minFreq = kMinDisplayFreq;                         // 例如 80 Hz
    const float maxFreq = juce::jmin (kMaxDisplayFreq, nyquist);   // 例如 5000 Hz 或 Nyquist
    const float melMin  = hzToMel (minFreq);
    const float melMax  = hzToMel (maxFreq);

    const int x = width - 1;   // 新的一列在最右侧

    const float minDb = -96.0f;
    const float maxDb =  -6.0f;
    // JUCE's frequency-only FFT is unscaled. The Hann table is normalised to
    // unity DC gain, so a bin-centred sine has a magnitude of N / 2. Convert
    // that raw magnitude to peak-amplitude dBFS before applying the display
    // range; otherwise typical signals exceed maxDb by roughly 60 dB and the
    // entire spectrogram saturates at the hottest colour.
    const float fftMagnitudeToDbfs = juce::Decibels::gainToDecibels (
        2.0f / (float) kFftSize,
        -120.0f);

    for (int y = 0; y < height; ++y)
    {
        float normY = 1.0f - (float) y / (float) (height - 1);
        float mel   = melMin + normY * (melMax - melMin);
        float freq  = melToHz (mel);

        float binPos = (freq / nyquist) * (float) (numBins - 1);
        int   bin    = (int) std::round (binPos);
        bin          = juce::jlimit (0, numBins - 1, bin);

        float db = spectrumForDrawing[(size_t) bin];
        if (std::isnan (db) || std::isinf (db))
            db = minDb;
        else
            db += fftMagnitudeToDbfs;

        db = juce::jlimit (minDb, maxDb, db);

        float norm = (db - minDb) / (maxDb - minDb);
        const auto colour = spectrogramColourForLevel (norm);
        spectrogramImage.setPixelAt (x, y, colour);
    }

    if (pendingFreezeMarker)
    {
        for (int y = 0; y < height; ++y)
        {
            const auto baseColour = spectrogramImage.getPixelAt (x, y);
            spectrogramImage.setPixelAt (x, y, baseColour.interpolatedWith (kFreezeMarker, 0.72f));
        }

        pendingFreezeMarker = false;
    }
}

//==============================================================================
void AudioPluginAudioProcessorEditor::timerCallback()
{
    processorRef.getSpectrumCopy (latestSpectrum);
    processorRef.getResidualCopy (latestResidual);
    processorRef.getTopPeaksCopy (latestTopFreqs,
                                  latestTopMags,
                                  latestTopPeakLevelsDbFs);

    spectrumForDrawing = latestSpectrum;
    topFreqsForDrawing = latestTopFreqs;
    topMagsForDrawing  = latestTopMags;
    topPeakLevelsDbFsForDrawing = latestTopPeakLevelsDbFs;

    const double nowSeconds = juce::Time::getMillisecondCounterHiRes() * 0.001;

    // Start remains a deliberate user action. Once host playback has been
    // observed, however, a Playing -> Stopped transition performs the same
    // finalisation as the Stop button before a silent descriptor frame can be
    // appended. Standalone operation has no host state and remains manual.
    if (isAutoRunning && processorRef.hasHostTransportState())
    {
        const bool hostIsPlaying = processorRef.getHostTransportPlaying();

        if (hostIsPlaying)
            hasObservedHostPlaybackSinceAutoStart = true;
        else if (hasObservedHostPlaybackSinceAutoStart && previousHostTransportPlaying)
            finishAutoAnalysis (nowSeconds, "DAW transport stopped. Analysis complete.", false);

        previousHostTransportPlaying = hostIsPlaying;
    }

    recordDetailAnalysisFrame (nowSeconds);
    processAutoOnsetDetection (nowSeconds);
    pushSpectrumToImage();

    if (staffComponent != nullptr)
        staffComponent->repaint();

    repaint();
}
