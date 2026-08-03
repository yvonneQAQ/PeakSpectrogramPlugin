#include "PluginProcessor.h"
#include "PluginEditor.h"


#include <cmath> // 为了 std::log2, std::round
#include <cstring>
#include <limits>
#include <utility>


namespace
{
    constexpr int kMaxFrozenChords = 20;
    constexpr int kMaxDetailAnalysisFrames = 2400;

    const juce::Colour kEditorBackground = juce::Colour::fromRGB (156, 167, 127);
    const juce::Colour kPanelBackground = juce::Colour::fromRGB (41, 56, 49);
    const juce::Colour kPanelOutline = juce::Colour::fromRGB (110, 125, 93);
    const juce::Colour kPanelTint = juce::Colour::fromRGB (170, 180, 141);
    const juce::Colour kTextPrimary = juce::Colour::fromRGB (24, 28, 20);
    const juce::Colour kTextSecondary = juce::Colour::fromRGB (55, 66, 46);
    const juce::Colour kButtonTerracotta = juce::Colour::fromRGB (178, 97, 76);
    const juce::Colour kButtonTerracottaStrong = juce::Colour::fromRGB (156, 78, 59);
    const juce::Colour kButtonTerracottaMuted = juce::Colour::fromRGB (193, 126, 108);
    const juce::Colour kButtonText = juce::Colour::fromRGB (248, 241, 234);
    const juce::Colour kSpectrogramBase = juce::Colour::fromRGB (20, 34, 30);
    const juce::Colour kSpectrogramLine = juce::Colour::fromRGB (238, 243, 233).withAlpha (0.26f);
    const juce::Colour kSpectrogramLabel = juce::Colour::fromRGB (245, 247, 241).withAlpha (0.94f);

    juce::Colour spectrogramColourForLevel (float norm)
    {
        norm = juce::jlimit (0.0f, 1.0f, norm);

        const auto low = juce::Colour::fromRGB (30, 48, 41);
        const auto mid = juce::Colour::fromRGB (72, 116, 103);
        const auto high = juce::Colour::fromRGB (220, 210, 172);

        if (norm < 0.55f)
            return low.interpolatedWith (mid, norm / 0.55f);

        return mid.interpolatedWith (high, (norm - 0.55f) / 0.45f);
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
                           .withHeight (height)
                           .withFallbackEnabled (true)
                           .withFallbacks ({ "Apple Symbols", "Arial Unicode MS", "Arial Unicode" });

        if (auto typeface = getMusicTypeface())
            options = options.withTypeface (typeface);

        return juce::Font (options);
    }

    juce::Font makeUIFont (float height, bool bold = false)
    {
        auto options = juce::FontOptions {}
                           .withName ("STHeiti SC")
                           .withHeight (height)
                           .withFallbackEnabled (true)
                           .withFallbacks ({ "Heiti SC", "PingFang SC", "Hiragino Sans GB", "Helvetica Neue" });

        if (bold)
            options = options.withStyle ("Medium");

        return juce::Font (options);
    }

    juce::Font makeTitleFont (float height)
    {
        auto options = juce::FontOptions {}
                           .withName ("DIN Alternate")
                           .withHeight (height)
                           .withFallbackEnabled (true)
                           .withFallbacks ({ "DIN Condensed", "Avenir Next Condensed", "Helvetica Neue" });

        return juce::Font (options).boldened();
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
        juce::Font getTextButtonFont (juce::TextButton&, int buttonHeight) override
        {
            return makeUIFont ((float) buttonHeight * 0.36f, true);
        }

        void drawButtonBackground (juce::Graphics& g,
                                   juce::Button& button,
                                   const juce::Colour&,
                                   bool shouldDrawButtonAsHighlighted,
                                   bool shouldDrawButtonAsDown) override
        {
            auto bounds = button.getLocalBounds().toFloat().reduced (0.5f);
            auto colour = button.getToggleState() ? kButtonTerracottaStrong : kButtonTerracotta;

            if (! button.isEnabled())
                colour = kButtonTerracotta.withMultipliedAlpha (0.45f);
            else if (shouldDrawButtonAsDown)
                colour = colour.darker (0.12f);
            else if (shouldDrawButtonAsHighlighted)
                colour = colour.brighter (0.06f);

            const auto cornerSize = bounds.getHeight() * 0.48f;
            auto shadowBounds = bounds.translated (0.0f, 1.3f);
            g.setColour (juce::Colours::black.withAlpha (button.isEnabled() ? 0.11f : 0.05f));
            g.fillRoundedRectangle (shadowBounds, cornerSize);

            g.setColour (colour);
            g.fillRoundedRectangle (bounds, cornerSize);

            g.setColour (juce::Colours::white.withAlpha (0.10f));
            g.drawRoundedRectangle (bounds.reduced (0.8f), cornerSize, 0.9f);
        }

        void drawButtonText (juce::Graphics& g,
                             juce::TextButton& button,
                             bool,
                             bool) override
        {
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
        const auto offColour = emphasised ? kButtonTerracottaStrong : kButtonTerracotta;
        const auto onColour = kButtonTerracottaMuted;

        button.setColour (juce::TextButton::buttonColourId, offColour);
        button.setColour (juce::TextButton::buttonOnColourId, onColour);
        button.setColour (juce::TextButton::textColourOffId, kButtonText);
        button.setColour (juce::TextButton::textColourOnId, kButtonText);
    }

    struct PitchNotation
    {
        bool valid = false;
        juce::String displayLabel;
        juce::String accidentalForStaff;
        int diatonicNumber = 0;
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

        for (float freqHz : snapshot.freqsHz)
        {
            const auto notation = quantiseFrequencyToPitchNotation (freqHz, snapshot.useQuarterToneMode);
            if (notation.valid)
                notes.push_back (notation);
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
    };

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

    bool containsMidiNote (const std::vector<QuantisedMidiNote>& notes,
                           int midiNote,
                           int pitchWheelValue)
    {
        return std::any_of (notes.begin(), notes.end(),
                            [midiNote, pitchWheelValue] (const auto& note)
                            {
                                return note.midiNote == midiNote
                                    && note.pitchWheelValue == pitchWheelValue;
                            });
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

    void appendFrozenChordEvents (juce::MidiMessageSequence& eventTrack,
                                  const std::vector<FrozenChordSnapshot>& snapshots)
    {
        constexpr int velocity = 96;

        for (int channel = 1; channel <= AudioPluginAudioProcessor::kNumNoisyPeaks; ++channel)
            appendPitchBendRangeSetup (eventTrack, channel, 0.0);

        for (size_t chordIndex = 0; chordIndex < snapshots.size(); ++chordIndex)
        {
            const double startBeat = snapshots[chordIndex].startTimeSeconds;
            const double endBeat = juce::jmax (snapshots[chordIndex].startTimeSeconds + 0.05,
                                               snapshots[chordIndex].endTimeSeconds);

            std::vector<QuantisedMidiNote> quantisedNotes;
            quantisedNotes.reserve (snapshots[chordIndex].freqsHz.size());

            for (float freqHz : snapshots[chordIndex].freqsHz)
            {
                const auto midiNote = quantiseFrequencyToMidiNote (freqHz, snapshots[chordIndex].useQuarterToneMode);
                if (! midiNote.valid)
                    continue;

                if (containsMidiNote (quantisedNotes, midiNote.midiNote, midiNote.pitchWheelValue))
                    continue;

                quantisedNotes.push_back (midiNote);
            }

            if (snapshots[chordIndex].useQuarterToneMode)
            {
                int channel = 1;
                for (const auto& note : quantisedNotes)
                {
                    eventTrack.addEvent (juce::MidiMessage::pitchWheel (channel, note.pitchWheelValue),
                                         startBeat * kMidiTicksPerQuarter);
                    eventTrack.addEvent (juce::MidiMessage::noteOn (channel, note.midiNote, (juce::uint8) velocity),
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
                    eventTrack.addEvent (juce::MidiMessage::noteOn (1, note.midiNote, (juce::uint8) velocity),
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
                                 const std::vector<DetailAnalysisFrame>& frames)
    {
        constexpr int midiChannel = 1;
        constexpr int centroidCc = 20;
        constexpr int flatnessCc = 21;
        constexpr int roughnessCc = 22;
        constexpr int stereoPanCc = 23;

        int previousCentroid = -1;
        int previousFlatness = -1;
        int previousRoughness = -1;
        int previousPan = -1;

        for (const auto& frame : frames)
        {
            const double tick = juce::jmax (0.0, frame.timeSeconds) * kMidiTicksPerQuarter;
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

    juce::File createMidiFileForSnapshots (const std::vector<FrozenChordSnapshot>& snapshots)
    {
        juce::MidiMessageSequence eventTrack;
        appendFrozenChordEvents (eventTrack, snapshots);
        return writeMidiFile ("FrozenChords", eventTrack);
    }

    juce::File createDescriptorMidiFile (const std::vector<DetailAnalysisFrame>& frames)
    {
        juce::MidiMessageSequence eventTrack;
        appendDescriptorEvents (eventTrack, frames);
        return writeMidiFile ("SPEKANA_Descriptors", eventTrack);
    }

    juce::File createCombinedMidiFile (const std::vector<FrozenChordSnapshot>& snapshots,
                                       const std::vector<DetailAnalysisFrame>& frames)
    {
        juce::MidiMessageSequence eventTrack;
        appendFrozenChordEvents (eventTrack, snapshots);
        appendDescriptorEvents (eventTrack, frames);
        eventTrack.updateMatchedPairs();
        return writeMidiFile ("SPEKANA_Combined", eventTrack);
    }
}

class FrozenChordStaffComponent : public juce::Component
{
public:
    void setSnapshots (std::vector<FrozenChordSnapshot> newSnapshots)
    {
        snapshots = std::move (newSnapshots);
        repaint();
    }

    void paint (juce::Graphics& g) override
    {
        auto bounds = getLocalBounds().reduced (10);
        auto header = bounds.removeFromTop (26);

        header.removeFromLeft (72);

        dragMidiBounds = header.removeFromRight (134).translated (0, -10).reduced (2, 0);
        g.setColour (snapshots.empty() ? kButtonTerracotta.withMultipliedAlpha (0.42f)
                                       : kButtonTerracotta);
        g.fillRoundedRectangle (dragMidiBounds.toFloat(), 10.0f);
        g.setColour (kButtonText);
        g.setFont (makeUIFont (10.8f, true));
        g.drawText ("Drag MIDI", dragMidiBounds, juce::Justification::centred);

        auto systemBounds = bounds.reduced (0, 2);
        drawContinuousSystem (g, systemBounds);
    }

    void mouseDown (const juce::MouseEvent& event) override
    {
        dragTriggered = false;
        dragCandidate = dragMidiBounds.contains (event.getPosition());
    }

    void mouseDrag (const juce::MouseEvent& event) override
    {
        if (! dragCandidate || dragTriggered || snapshots.empty())
            return;

        if (event.getDistanceFromDragStart() < 6)
            return;

        auto midiFile = createMidiFileForSnapshots (snapshots);
        if (! midiFile.existsAsFile())
            return;

        dragTriggered = true;
        juce::DragAndDropContainer::performExternalDragDropOfFiles (
            juce::StringArray { midiFile.getFullPathName() },
            false,
            this);
    }

    void mouseUp (const juce::MouseEvent&) override
    {
        dragCandidate = false;
    }

private:
    struct DrawnNote
    {
        float x = 0.0f;
        float y = 0.0f;
        int diatonicNumber = 0;
    };

    std::vector<FrozenChordSnapshot> snapshots;
    juce::Rectangle<int> dragMidiBounds;
    bool dragCandidate = false;
    bool dragTriggered = false;

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
        const float step = availableWidth / (float) juce::jmax (1, kMaxFrozenChords - 1);
        const double nowSeconds = juce::Time::getMillisecondCounterHiRes() * 0.001;

        for (size_t i = 0; i < snapshots.size(); ++i)
        {
            const float x = startX + step * (float) i;
            const auto notes = chordToPitchNotation (snapshots[i]);
            if (notes.empty())
                continue;

            float animationProgress = 1.0f;
            if (snapshots[i].visualCreatedWallTimeSeconds > 0.0)
            {
                const auto elapsedSeconds = nowSeconds - snapshots[i].visualCreatedWallTimeSeconds;
                animationProgress = juce::jlimit (0.0f, 1.0f, (float) (elapsedSeconds / 0.28));
            }

            const float easedProgress = 1.0f - std::pow (1.0f - animationProgress, 3.0f);
            const float chordAlpha = 0.24f + 0.76f * easedProgress;

            if (animationProgress < 1.0f)
            {
                const float glowAlpha = (1.0f - animationProgress) * 0.18f;
                auto glowBounds = juce::Rectangle<float> (x - 15.0f,
                                                          (float) content.getY() + 8.0f,
                                                          32.0f,
                                                          (float) content.getHeight() - 16.0f);
                g.setColour (kButtonTerracottaStrong.withAlpha (glowAlpha));
                g.fillRoundedRectangle (glowBounds, 13.0f);
            }

            drawChord (g, content, notes, x, chordAlpha);

            if (i + 1 < (size_t) kMaxFrozenChords)
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
                    float centerX,
                    float alpha = 1.0f) const
    {
        const float noteHeadWidth = 5.8f;
        const float noteHeadHeight = 3.9f;
        alpha = juce::jlimit (0.0f, 1.0f, alpha);

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

            g.setColour (kTextPrimary.withAlpha (alpha));
            drawLedgerLines (g, area, noteX, note.diatonicNumber, alpha);

            if (note.accidentalForStaff == "q#" || note.accidentalForStaff == "qb")
            {
                g.setColour (kTextPrimary.withAlpha (alpha));
                drawQuarterToneArrowMark (g,
                                          juce::Rectangle<float> (noteX - 10.0f, noteY - 5.5f, 6.0f, 11.0f),
                                          note.accidentalForStaff == "q#");
            }
            else
            {
                const auto accidentalGlyph = accidentalGlyphForStaff (note.accidentalForStaff);
                if (accidentalGlyph.isNotEmpty())
                {
                    g.setColour (kTextPrimary.withAlpha (alpha));
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
            g.setColour (kTextPrimary.withAlpha (alpha));
            g.fillPath (noteHead);

            drawnNotes.push_back ({ noteX, noteY, note.diatonicNumber });

            previousDiatonic = note.diatonicNumber;
            ++clusterIndex;
        }

        if (drawnNotes.empty())
            return;

        const bool stemDown = drawnNotes.back().diatonicNumber >= 34;

        if (stemDown)
        {
            const auto& anchor = drawnNotes.back();
            const float stemX = anchor.x + 0.8f;
            g.setColour (kTextPrimary.withAlpha (alpha));
            g.drawLine (stemX, anchor.y, stemX, anchor.y + 14.4f, 1.0f);
        }
        else
        {
            const auto& anchor = drawnNotes.front();
            const float stemX = anchor.x + noteHeadWidth - 0.8f;
            g.setColour (kTextPrimary.withAlpha (alpha));
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

    void paint (juce::Graphics& g) override
    {
        const auto bounds = getLocalBounds().toFloat();
        const bool enabled = ! frames.empty();
        g.setColour (enabled ? kButtonTerracotta : kButtonTerracotta.withMultipliedAlpha (0.42f));
        g.fillRoundedRectangle (bounds, 9.0f);
        g.setColour (kButtonText);
        g.setFont (makeUIFont (8.6f, true));
        g.drawText (snapshots.empty() ? "Drag Descriptor MIDI" : "Drag Combined MIDI",
                    getLocalBounds(),
                    juce::Justification::centred);
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
                                ? createDescriptorMidiFile (frames)
                                : createCombinedMidiFile (snapshots, frames);
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

    setSize (860, 520);
    spectrogramImage = juce::Image (juce::Image::RGB, 420, 220, true);
    clearSpectrogramImage();

    startTimerHz (30);

    addAndMakeVisible (freezeButton);
    addAndMakeVisible (unfreezeButton);
    addAndMakeVisible (resetButton);
    addAndMakeVisible (bassBoostButton);
    addAndMakeVisible (quarterToneButton);
    addAndMakeVisible (exportMidiButton);
    addAndMakeVisible (manualModeButton);
    addAndMakeVisible (autoModeButton);
    addAndMakeVisible (autoStartButton);
    addAndMakeVisible (autoStopButton);
    addAndMakeVisible (autoClearButton);
    addAndMakeVisible (detailAnalysisButton);
    addAndMakeVisible (topPeakCountSlider);
    addAndMakeVisible (autoSensitivitySlider);
    addAndMakeVisible (autoMinGapSlider);

    staffComponent = std::make_unique<FrozenChordStaffComponent>();
    addAndMakeVisible (*staffComponent);

    descriptorMidiDragComponent = std::make_unique<DescriptorMidiDragComponent> (detailAnalysisHistory,
                                                                                  frozenChords);
    addAndMakeVisible (*descriptorMidiDragComponent);
    descriptorMidiDragComponent->setVisible (false);

    styleTextButton (freezeButton, true);
    styleTextButton (unfreezeButton);
    styleTextButton (resetButton);
    styleTextButton (bassBoostButton);
    styleTextButton (quarterToneButton);
    styleTextButton (exportMidiButton);
    styleTextButton (manualModeButton);
    styleTextButton (autoModeButton);
    styleTextButton (autoStartButton, true);
    styleTextButton (autoStopButton);
    styleTextButton (autoClearButton);
    styleTextButton (detailAnalysisButton);

    manualModeButton.setClickingTogglesState (true);
    autoModeButton.setClickingTogglesState (true);
    manualModeButton.onClick = [this]() { setAutoPageActive (false); };
    autoModeButton.onClick = [this]() { setAutoPageActive (true); };

    autoStartButton.onClick = [this]()
    {
        isDetailPageActive = false;
        isAutoRunning = true;
        autoStartWallTimeSeconds = juce::Time::getMillisecondCounterHiRes() * 0.001;
        detailAnalysisHistory.clear();
        hasCompletedAutoAnalysis = false;
        resetAutoDetectorState();
        refreshAutoButtonStates();
        refreshDetailAnalysisButtonState();
        refreshExportButtonState();
        showTransientStatusMessage ("Auto listening for onsets.");
        repaint();
    };

    autoStopButton.onClick = [this]()
    {
        const double nowSeconds = juce::Time::getMillisecondCounterHiRes() * 0.001;
        if (! frozenChords.empty() && frozenChords.back().label.startsWith ("Auto "))
            frozenChords.back().endTimeSeconds = juce::jmax (frozenChords.back().startTimeSeconds + 0.05,
                                                             nowSeconds - autoStartWallTimeSeconds);

        isAutoRunning = false;
        hasCompletedAutoAnalysis = ! detailAnalysisHistory.empty();
        refreshStaffComponent();
        refreshAutoButtonStates();
        refreshDetailAnalysisButtonState();
        showTransientStatusMessage ("Auto stopped.");
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

        repaint();
    };

    topPeakCountSlider.setRange (1.0, (double) kNumNoisyPeaks, 1.0);
    topPeakCountSlider.setValue ((double) kNumNoisyPeaks, juce::dontSendNotification);
    topPeakCountSlider.setSliderStyle (juce::Slider::LinearHorizontal);
    topPeakCountSlider.setTextBoxStyle (juce::Slider::TextBoxRight, false, 30, 16);
    topPeakCountSlider.setColour (juce::Slider::trackColourId, kButtonTerracotta);
    topPeakCountSlider.setColour (juce::Slider::thumbColourId, kButtonTerracottaStrong);
    topPeakCountSlider.setColour (juce::Slider::backgroundColourId, kPanelTint.withMultipliedAlpha (0.35f));
    topPeakCountSlider.setColour (juce::Slider::textBoxTextColourId, kTextPrimary);
    topPeakCountSlider.setColour (juce::Slider::textBoxOutlineColourId, juce::Colours::transparentBlack);
    topPeakCountSlider.onValueChange = [this]()
    {
        repaint();
    };

    autoSensitivitySlider.setRange (1.0, 10.0, 0.5);
    autoSensitivitySlider.setValue (7.0, juce::dontSendNotification);
    autoSensitivitySlider.setSliderStyle (juce::Slider::LinearHorizontal);
    autoSensitivitySlider.setTextBoxStyle (juce::Slider::TextBoxRight, false, 34, 16);
    autoSensitivitySlider.setColour (juce::Slider::trackColourId, kButtonTerracotta);
    autoSensitivitySlider.setColour (juce::Slider::thumbColourId, kButtonTerracottaStrong);
    autoSensitivitySlider.setColour (juce::Slider::backgroundColourId, kPanelTint.withMultipliedAlpha (0.35f));
    autoSensitivitySlider.setColour (juce::Slider::textBoxTextColourId, kTextPrimary);
    autoSensitivitySlider.setColour (juce::Slider::textBoxOutlineColourId, juce::Colours::transparentBlack);

    autoMinGapSlider.setRange (80.0, 600.0, 10.0);
    autoMinGapSlider.setValue (180.0, juce::dontSendNotification);
    autoMinGapSlider.setSliderStyle (juce::Slider::LinearHorizontal);
    autoMinGapSlider.setTextBoxStyle (juce::Slider::TextBoxRight, false, 34, 16);
    autoMinGapSlider.setColour (juce::Slider::trackColourId, kButtonTerracotta);
    autoMinGapSlider.setColour (juce::Slider::thumbColourId, kButtonTerracottaStrong);
    autoMinGapSlider.setColour (juce::Slider::backgroundColourId, kPanelTint.withMultipliedAlpha (0.35f));
    autoMinGapSlider.setColour (juce::Slider::textBoxTextColourId, kTextPrimary);
    autoMinGapSlider.setColour (juce::Slider::textBoxOutlineColourId, juce::Colours::transparentBlack);

    bassBoostButton.setClickingTogglesState (true);
    bassBoostButton.setToggleState (processorRef.getBassBoostMode(), juce::dontSendNotification);
    refreshBassBoostButtonText();
    bassBoostButton.onClick = [this]()
    {
        processorRef.setBassBoostMode (bassBoostButton.getToggleState());
        refreshBassBoostButtonText();
        repaint();
    };

    quarterToneButton.setClickingTogglesState (true);
    quarterToneButton.onClick = [this]()
    {
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

    spectrumForDrawing = latestSpectrum;
    topFreqsForDrawing = latestTopFreqs;
    topMagsForDrawing  = latestTopMags;

    pendingFreezeMarker = true;
    pushSpectrumToImage();
    captureFrozenChord();

    repaint();
};



unfreezeButton.onClick = [this]()
{
    closeActiveFrozenChord();
    isFrozen = false;
    processorRef.clearLiveFrozenMidiChord();
    repaint();
};

    resetButton.onClick = [this]()
    {
        resetFrozenState();
    };

    exportMidiButton.onClick = [this]()
    {
        const bool includeDescriptors = hasCompletedAutoAnalysis
                                     && ! detailAnalysisHistory.empty();
        auto midiFile = includeDescriptors
                          ? createCombinedMidiFile (frozenChords, detailAnalysisHistory)
                          : createMidiFileForSnapshots (frozenChords);
        if (! midiFile.existsAsFile())
            return;

        exportMidiChooser = std::make_unique<juce::FileChooser> (
            includeDescriptors ? "Export Combined Notes and Descriptors MIDI"
                               : "Export Frozen Chords MIDI",
            juce::File::getSpecialLocation (juce::File::userDocumentsDirectory)
                .getNonexistentChildFile (includeDescriptors ? "SPEKANA_Combined"
                                                             : "FrozenChords",
                                          ".mid",
                                          false),
            "*.mid");

        exportMidiChooser->launchAsync (juce::FileBrowserComponent::saveMode
                                            | juce::FileBrowserComponent::canSelectFiles,
                                        [this, midiFile] (const juce::FileChooser& chooser)
                                        {
                                            auto target = chooser.getResult();
                                            if (target == juce::File())
                                            {
                                                exportMidiChooser.reset();
                                                return;
                                            }

                                            if (target.existsAsFile())
                                                target.deleteFile();

                                            midiFile.copyFileTo (target);
                                            exportMidiChooser.reset();
                                        });
    };

    refreshExportButtonState();
    refreshModeButtonStates();
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
    return quarterToneButton.getToggleState();
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

void AudioPluginAudioProcessorEditor::setAutoPageActive (bool shouldUseAutoPage)
{
    if (! shouldUseAutoPage && isAutoRunning)
    {
        const double nowSeconds = juce::Time::getMillisecondCounterHiRes() * 0.001;
        if (! frozenChords.empty() && frozenChords.back().label.startsWith ("Auto "))
            frozenChords.back().endTimeSeconds = juce::jmax (frozenChords.back().startTimeSeconds + 0.05,
                                                             nowSeconds - autoStartWallTimeSeconds);

        isAutoRunning = false;
        hasCompletedAutoAnalysis = ! detailAnalysisHistory.empty();
        processorRef.clearLiveFrozenMidiChord();
        refreshAutoButtonStates();
        refreshDetailAnalysisButtonState();
    }

    isAutoPageActive = shouldUseAutoPage;
    isDetailPageActive = false;
    refreshModeButtonStates();
    refreshDetailAnalysisButtonState();
    refreshStaffComponent();
    resized();
    repaint();
}

void AudioPluginAudioProcessorEditor::refreshModeButtonStates()
{
    manualModeButton.setToggleState (! isAutoPageActive, juce::dontSendNotification);
    autoModeButton.setToggleState (isAutoPageActive, juce::dontSendNotification);

    freezeButton.setVisible (! isAutoPageActive);
    unfreezeButton.setVisible (! isAutoPageActive);
    resetButton.setVisible (! isAutoPageActive);
    quarterToneButton.setVisible (true);
    exportMidiButton.setVisible (true);

    autoStartButton.setVisible (isAutoPageActive);
    autoStopButton.setVisible (isAutoPageActive);
    autoClearButton.setVisible (isAutoPageActive);
    autoSensitivitySlider.setVisible (isAutoPageActive);
    autoMinGapSlider.setVisible (isAutoPageActive);
}

void AudioPluginAudioProcessorEditor::refreshAutoButtonStates()
{
    autoStartButton.setEnabled (! isAutoRunning);
    autoStopButton.setEnabled (isAutoRunning);
    autoClearButton.setEnabled (! frozenChords.empty() || autoCaptureCount > 0);
}

void AudioPluginAudioProcessorEditor::resetAutoDetectorState()
{
    previousAutoResidual.fill (0.0f);
    hasPreviousAutoResidual = false;
    hasPreviousAutoInputLevel = false;
    autoFluxMean = 0.0f;
    autoFluxDeviation = 0.18f;
    autoOnsetStrength = 0.0f;
    previousAutoInputLevelDb = -120.0f;
    lastAutoOnsetWallTimeSeconds = -10.0;
}

DetailAnalysisFrame AudioPluginAudioProcessorEditor::calculateDetailAnalysisFrame (double nowSeconds) const
{
    DetailAnalysisFrame frame;
    frame.timeSeconds = juce::jmax (0.0, nowSeconds - autoStartWallTimeSeconds);
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
    double magnitudeSum = 0.0;
    double weightedFrequencySum = 0.0;
    int magnitudeCount = 0;

    for (int bin = minBin; bin <= maxBin; ++bin)
    {
        const float db = juce::jlimit (-120.0f, 24.0f, latestSpectrum[(size_t) bin]);
        const double magnitude = juce::jmax (1.0e-12, std::pow (10.0, (double) db / 20.0));
        const double hz = (double) bin * sampleRate / (double) kFftSize;

        logMagnitudeSum += std::log (magnitude);
        magnitudeSum += magnitude;
        weightedFrequencySum += hz * magnitude;
        ++magnitudeCount;
    }

    if (magnitudeCount > 0 && magnitudeSum > 1.0e-12)
    {
        const double geometricMean = std::exp (logMagnitudeSum / (double) magnitudeCount);
        const double arithmeticMean = magnitudeSum / (double) magnitudeCount;
        frame.spectralFlatness = (float) juce::jlimit (0.0, 1.0, geometricMean / arithmeticMean);
        frame.centroidHz = (float) juce::jlimit (0.0, (double) sampleRate * 0.5,
                                                weightedFrequencySum / magnitudeSum);
    }

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
}

void AudioPluginAudioProcessorEditor::captureAutoChord (double nowSeconds)
{
    if ((int) frozenChords.size() >= kMaxFrozenChords)
    {
        isAutoRunning = false;
        processorRef.clearLiveFrozenMidiChord();
        showTransientStatusMessage ("20 chords max reached. Press Clear to start a new score.");
        refreshAutoButtonStates();
        refreshStaffComponent();
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

    snapshot.useQuarterToneMode = useQuarterToneMode();
    snapshot.startTimeSeconds = startTimeSeconds;
    snapshot.endTimeSeconds = startTimeSeconds + 0.25;
    snapshot.visualCreatedWallTimeSeconds = nowSeconds;

    ++autoCaptureCount;
    ++freezeCaptureCount;
    snapshot.label = "Auto " + juce::String (autoCaptureCount);

    frozenChords.push_back (snapshot);
    processorRef.setLiveFrozenMidiChord (snapshot.freqsHz, snapshot.useQuarterToneMode);
    pendingFreezeMarker = true;

    refreshStaffComponent();
    refreshAutoButtonStates();
}

void AudioPluginAudioProcessorEditor::processAutoOnsetDetection (double nowSeconds)
{
    if (! isAutoPageActive || ! isAutoRunning)
        return;

    if ((int) frozenChords.size() >= kMaxFrozenChords)
    {
        isAutoRunning = false;
        processorRef.clearLiveFrozenMidiChord();
        refreshAutoButtonStates();
        showTransientStatusMessage ("20 chords max reached. Auto stopped.");
        repaint();
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

            const float sensitivity = (float) autoSensitivitySlider.getValue();
            const float thresholdScale = juce::jmap (sensitivity, 1.0f, 10.0f, 3.2f, 0.65f);
            threshold = autoFluxMean + thresholdScale * juce::jmax (0.18f, autoFluxDeviation);
            spectralOnsetDetected = autoOnsetStrength > threshold && autoOnsetStrength > 0.28f;
        }
    }

    previousAutoResidual = latestResidual;
    hasPreviousAutoResidual = true;

    const double minGapSeconds = autoMinGapSlider.getValue() * 0.001;
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
    bassBoostButton.setButtonText (bassBoostButton.getToggleState()
                                     ? "Bass Boost: On"
                                     : "Bass Boost: Off");
}

void AudioPluginAudioProcessorEditor::captureFrozenChord()
{
    const double nowSeconds = juce::Time::getMillisecondCounterHiRes() * 0.001;

    if ((int) frozenChords.size() >= kMaxFrozenChords)
    {
        isFrozen = false;
        processorRef.clearLiveFrozenMidiChord();
        showTransientStatusMessage ("20 chords max reached. Press Reset to start a new score.");
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

    snapshot.useQuarterToneMode = useQuarterToneMode();
    snapshot.startTimeSeconds = activeFreezeSessionOffsetSeconds
                              + (nowSeconds - activeFreezeSessionStartWallTimeSeconds);
    snapshot.endTimeSeconds = snapshot.startTimeSeconds + 0.25;
    snapshot.visualCreatedWallTimeSeconds = nowSeconds;

    ++freezeCaptureCount;
    snapshot.label = "Freeze " + juce::String (freezeCaptureCount);

    frozenChords.push_back (snapshot);
    processorRef.setLiveFrozenMidiChord (snapshot.freqsHz, snapshot.useQuarterToneMode);

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
    exportMidiButton.setButtonText (hasCompletedAutoAnalysis
                                     && ! detailAnalysisHistory.empty()
                                      ? "Export Combined"
                                      : "Export MIDI");
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
    latestSpectrum.fill (0.0f);
    latestResidual.fill (0.0f);
    previousAutoResidual.fill (0.0f);
    latestTopFreqs.fill (0.0f);
    latestTopMags.fill (0.0f);

    clearSpectrogramImage();
    resetAutoDetectorState();

    refreshStaffComponent();
    refreshAutoButtonStates();
    refreshDetailAnalysisButtonState();

    repaint();
}

void AudioPluginAudioProcessorEditor::clearSpectrogramImage()
{
    spectrogramImage.clear (spectrogramImage.getBounds(), kSpectrogramBase);
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
    auto panel = area.toFloat();
    g.setColour (kPanelTint.withMultipliedAlpha (available ? 0.22f : 0.10f));
    g.fillRoundedRectangle (panel, 12.0f);
    g.setColour (kTextPrimary.withAlpha (available ? 0.22f : 0.10f));
    g.drawRoundedRectangle (panel.reduced (0.5f), 12.0f, 1.0f);

    auto header = area.reduced (10, 8).removeFromTop (18);
    g.setFont (makeUIFont (9.0f, true));
    g.setColour (available ? kTextPrimary : kTextSecondary.withAlpha (0.45f));
    g.drawText (title, header, juce::Justification::centredLeft);

    g.setFont (makeUIFont (7.6f, false));
    g.drawText (available ? valueText : "Unavailable",
                header,
                juce::Justification::centredRight);

    auto graph = area.reduced (12, 10);
    graph.removeFromTop (24);

    g.setColour (kTextPrimary.withAlpha (0.10f));
    for (int i = 1; i < 4; ++i)
    {
        const auto y = graph.getY() + graph.getHeight() * i / 4;
        g.drawHorizontalLine (y, (float) graph.getX(), (float) graph.getRight());
    }

    if (! available || detailAnalysisHistory.size() < 2 || maxValue <= minValue)
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

    g.setColour (kButtonTerracottaStrong.withAlpha (0.92f));
    g.strokePath (curve, juce::PathStrokeType (1.7f));
}

void AudioPluginAudioProcessorEditor::drawDetailAnalysisPanel (juce::Graphics& g, juce::Rectangle<int> area)
{
    auto panel = area.reduced (10, 8);
    g.setColour (kPanelBackground.withAlpha (0.95f));
    g.fillRoundedRectangle (panel.toFloat(), 16.0f);

    auto header = panel.reduced (14, 10).removeFromTop (24);
    auto headerTextArea = header.withTrimmedRight (158);
    g.setColour (kSpectrogramLabel);
    g.setFont (makeUIFont (12.0f, true));
    g.drawText ("Detail Analysis", headerTextArea, juce::Justification::centredLeft);

    g.setFont (makeUIFont (8.0f, false));
    const auto duration = detailAnalysisHistory.empty() ? 0.0 : detailAnalysisHistory.back().timeSeconds;
    g.drawText (juce::String (detailAnalysisHistory.size()) + " frames  |  "
                + juce::String (duration, 1) + " sec",
                headerTextArea,
                juce::Justification::centredRight);

    if (detailAnalysisHistory.empty())
    {
        g.setColour (kSpectrogramLabel.withAlpha (0.70f));
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

    auto grid = panel.reduced (14, 42);
    const int gap = 10;
    const int cellWidth = (grid.getWidth() - gap) / 2;
    const int cellHeight = (grid.getHeight() - gap) / 2;

    auto flatnessArea = juce::Rectangle<int> (grid.getX(), grid.getY(), cellWidth, cellHeight);
    auto roughnessArea = juce::Rectangle<int> (grid.getX() + cellWidth + gap, grid.getY(), cellWidth, cellHeight);
    auto centroidArea = juce::Rectangle<int> (grid.getX(), grid.getY() + cellHeight + gap, cellWidth, cellHeight);
    auto panArea = juce::Rectangle<int> (grid.getX() + cellWidth + gap,
                                         grid.getY() + cellHeight + gap,
                                         cellWidth,
                                         cellHeight);

    drawAnalysisCurve (g,
                       flatnessArea,
                       "Spectral Flatness",
                       juce::String (latest.spectralFlatness, 3),
                       0.0f,
                       1.0f,
                       true,
                       [] (const auto& frame) { return frame.spectralFlatness; });

    drawAnalysisCurve (g,
                       roughnessArea,
                       "Roughness",
                       juce::String (latest.roughness, 3),
                       0.0f,
                       1.0f,
                       true,
                       [] (const auto& frame) { return frame.roughness; });

    drawAnalysisCurve (g,
                       centroidArea,
                       "Centroid",
                       juce::String (latest.centroidHz, 0) + " Hz",
                       0.0f,
                       24000.0f,
                       true,
                       [] (const auto& frame) { return frame.centroidHz; });

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

    if (! hasStereoPan)
    {
        g.setColour (kSpectrogramLabel.withAlpha (0.48f));
        g.setFont (makeUIFont (7.8f, false));
        g.drawText ("Mono input - no stereo field",
                    panArea.reduced (10, 30),
                    juce::Justification::centred);
    }
}

//==============================================================================
void AudioPluginAudioProcessorEditor::paint (juce::Graphics& g)
{
    g.fillAll (kEditorBackground);

    auto fullBounds = getLocalBounds();
    auto bounds = fullBounds.reduced (12);
    auto rightPanel = bounds.removeFromRight (206);
    bounds.removeFromRight (12);
    auto leftPanel = bounds;

    constexpr int spectrogramTopOffset = 36;
    constexpr int spectrogramBottomGap = 12;
    leftPanel.removeFromTop (spectrogramTopOffset);
    auto spectroArea = leftPanel.removeFromTop ((int) std::round (leftPanel.getHeight() * 0.39f));
    leftPanel.removeFromTop (spectrogramBottomGap);
    auto scoreArea = leftPanel;

    auto titleArea = rightPanel.removeFromTop (44);
    g.setColour (kTextPrimary);
    g.setFont (makeTitleFont (30.0f));
    auto buttonColumnBounds = freezeButton.getBounds()
                                .getUnion (unfreezeButton.getBounds())
                                .getUnion (resetButton.getBounds())
                                .getUnion (quarterToneButton.getBounds())
                                .getUnion (exportMidiButton.getBounds());
    auto titleBounds = juce::Rectangle<int> (buttonColumnBounds.getX() - 12,
                                             titleArea.getY(),
                                             buttonColumnBounds.getWidth() + 24,
                                             titleArea.getHeight());
    g.drawText ("SPEKANA",
                titleBounds,
                juce::Justification::centred);

    const double nowSeconds = juce::Time::getMillisecondCounterHiRes() * 0.001;

    if (transientStatusMessage.isNotEmpty() && nowSeconds >= transientStatusMessageExpirySeconds)
        {
            transientStatusMessage.clear();
        }

    auto rightInner = rightPanel.reduced (12);
    auto bottomInfoArea = rightInner.removeFromBottom (54);
    auto textArea = rightInner.removeFromBottom (148);
    textArea.removeFromTop (30);

    auto plotArea = spectroArea.reduced (8, 6);

    g.setColour (kPanelBackground);
    g.fillRoundedRectangle (plotArea.toFloat(), 14.0f);

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

            g.setColour (kSpectrogramLabel);
            g.drawFittedText (label, labelArea.toNearestInt(),
                              juce::Justification::centredLeft, 1);
            g.setColour (kSpectrogramLine);
        }
    }

    if (transientStatusMessage.isNotEmpty())
    {
        auto messageArea = plotArea.reduced (18, 12).removeFromBottom (24);
        g.setColour (kButtonTerracotta.withMultipliedAlpha (0.20f));
        g.fillRoundedRectangle (messageArea.toFloat(), 9.0f);
        g.setColour (kSpectrogramLabel);
        g.setFont (makeUIFont (8.2f, false));
        g.drawFittedText (transientStatusMessage,
                          messageArea.reduced (8, 4),
                          juce::Justification::centredLeft,
                          2);
    }

    if (isDetailPageActive)
        drawDetailAnalysisPanel (g, scoreArea);

    auto inner = textArea.reduced (2, 4);

    g.setColour (kTextPrimary);
    g.setFont (makeUIFont (10.2f, true));
    auto peaksTitleArea = inner.removeFromTop (20);
    const int activePeakCount = getActivePeakCount();
    g.drawText ("Top " + juce::String (activePeakCount) + " Peaks",
                peaksTitleArea,
                juce::Justification::centredLeft);

    g.setColour (kTextSecondary);
    g.setFont (makeUIFont (8.4f, false));
    g.drawText (useQuarterToneMode() ? "24-TET" : "12-TET",
                peaksTitleArea,
                juce::Justification::centredRight);

    inner.removeFromTop (2);
    auto peakCountControlArea = inner.removeFromTop (20);
    g.setColour (kTextSecondary);
    g.setFont (makeUIFont (7.4f, false));
    g.drawText ("Count",
                peakCountControlArea.removeFromLeft (36),
                juce::Justification::centredLeft);
    inner.removeFromTop (4);

    g.setFont (makeUIFont (7.8f, false));

    const int numCols       = 2;
    const int rowsPerColumn = juce::jmax (1, (activePeakCount + numCols - 1) / numCols);
    const int colWidth   = inner.getWidth()  / numCols;
    const int lineHeight = inner.getHeight() / rowsPerColumn;

    for (int i = 0; i < activePeakCount; ++i)
    {
        int col = i / rowsPerColumn;
        int row = i % rowsPerColumn;

        juce::Rectangle<int> cell;
        cell.setX (inner.getX() + col * colWidth);
        cell.setY (inner.getY() + row * lineHeight);
        cell.setWidth  (colWidth);
        cell.setHeight (lineHeight);

        float freq = topFreqsForDrawing[(size_t) i];
        juce::String text = juce::String (i + 1) + ". ";

        if (freq > 0.0f)
        {
            auto noteName = freqToPitchName (freq, useQuarterToneMode());
            text << noteName << "  " << juce::String (freq, 1) << " Hz";
        }
        else
        {
            text << "-";
        }

        g.setColour (kTextSecondary);
        g.drawText (text,
                    cell,
                    juce::Justification::centredLeft);
    }

    if (isAutoPageActive)
    {
        g.setColour (kTextSecondary);
        g.setFont (makeUIFont (7.2f, false));
        g.drawText ("Sensitivity",
                    autoSensitivitySlider.getBounds().translated (-74, 0).withWidth (70),
                    juce::Justification::centredRight);
        g.drawText ("Min Gap",
                    autoMinGapSlider.getBounds().translated (-74, 0).withWidth (70),
                    juce::Justification::centredRight);

        auto autoReadoutArea = juce::Rectangle<int> (autoClearButton.getX() - 14,
                                                     autoClearButton.getBottom() + 2,
                                                     autoClearButton.getWidth() + 28,
                                                     12);
        const auto stateText = isAutoRunning ? juce::String ("Auto running")
                                             : juce::String ("Auto stopped");
        g.drawText (stateText + "  |  " + juce::String (autoCaptureCount) + " captures",
                    autoReadoutArea,
                    juce::Justification::centred);
    }

    auto infoArea = bottomInfoArea.reduced (0, 2);
    infoArea.removeFromTop (6);
    auto modeRow = infoArea.removeFromBottom (12);
    g.setColour (kTextSecondary);
    g.setFont (makeUIFont (7.4f, false));
    g.drawText (bassBoostButton.getToggleState() ? "Analysis  |  Bass Boost On"
                                                 : "Analysis  |  Original",
                modeRow,
                juce::Justification::centredRight);

    auto hostRow = infoArea.removeFromBottom (12);
    g.setColour (kTextSecondary);
    g.setFont (makeUIFont (7.6f, false));
    const auto inputLevelDb = processorRef.getInputLevelDb();
    const auto inputLevelText = inputLevelDb <= -119.0f
                              ? juce::String ("Input -inf dB")
                              : "Input " + juce::String (inputLevelDb, 1) + " dB";

    g.drawText (inputLevelText + (useQuarterToneMode() ? "  |  24-TET"
                                                       : "  |  12-TET"),
                hostRow,
                juce::Justification::centredRight);

    auto firstFreezeRow = infoArea.removeFromBottom (12);
    g.setColour (kTextSecondary);
    g.setFont (makeUIFont (7.2f, false));
    g.drawText (firstFreezeTimeLabel.isNotEmpty()
                    ? "First Freeze: " + firstFreezeTimeLabel
                    : "First Freeze: -",
                firstFreezeRow,
                juce::Justification::centredRight);

    auto liveRow = infoArea.removeFromBottom (18);
    auto statusPill = liveRow.removeFromRight (78);
    const auto statusText = isAutoRunning ? juce::String ("AUTO")
                                          : (isFrozen ? juce::String ("FROZEN") : juce::String ("LIVE"));
    g.setColour ((isFrozen || isAutoRunning) ? kButtonTerracottaStrong
                                             : kButtonTerracotta);
    g.fillRoundedRectangle (statusPill.toFloat(), 8.0f);
    g.setColour (kButtonText);
    g.setFont (makeUIFont (8.6f, true));
    g.drawText (statusText,
                statusPill,
                juce::Justification::centred);
}


void AudioPluginAudioProcessorEditor::resized()
{
    auto full = getLocalBounds();
    auto bounds = full.reduced (12);

    auto rightPanel = bounds.removeFromRight (206);
    bounds.removeFromRight (12);
    auto leftPanel = bounds;

    constexpr int spectrogramTopOffset = 36;
    constexpr int spectrogramBottomGap = 12;
    leftPanel.removeFromTop (spectrogramTopOffset);
    auto spectroArea = leftPanel.removeFromTop ((int) std::round (leftPanel.getHeight() * 0.39f));
    leftPanel.removeFromTop (spectrogramBottomGap);
    auto scoreArea = leftPanel;

    auto graphArea = spectroArea.reduced (16, 14);
    const int w = juce::jmax (1, graphArea.getWidth());
    const int h = juce::jmax (1, graphArea.getHeight());
    spectrogramImage = juce::Image (juce::Image::RGB, w, h, true);
    clearSpectrogramImage();

    if (staffComponent != nullptr)
    {
        staffComponent->setBounds (scoreArea);
        staffComponent->setVisible (! isDetailPageActive);
    }

    if (descriptorMidiDragComponent != nullptr)
    {
        auto detailPanel = scoreArea.reduced (10, 8);
        descriptorMidiDragComponent->setBounds (detailPanel.getRight() - 150,
                                                detailPanel.getY() + 8,
                                                150,
                                                22);
        descriptorMidiDragComponent->setVisible (isDetailPageActive
                                                  && hasCompletedAutoAnalysis
                                                  && ! detailAnalysisHistory.empty()
                                                  && ! isAutoRunning);
        descriptorMidiDragComponent->toFront (false);
    }

    rightPanel.removeFromTop (44);
    auto rightInner = rightPanel.reduced (12);
    rightInner.removeFromTop (10);
    rightInner.removeFromBottom (54);
    rightInner.removeFromBottom (148);
    rightInner.removeFromBottom (8);

    auto controlArea = rightInner.removeFromTop (182);
    constexpr int buttonHeight = 26;

    auto modeRow = controlArea.removeFromTop (24);
    const int modeButtonWidth = (modeRow.getWidth() - 8) / 2;
    manualModeButton.setBounds (modeRow.removeFromLeft (modeButtonWidth));
    modeRow.removeFromLeft (8);
    autoModeButton.setBounds (modeRow);
    controlArea.removeFromTop (6);

    auto placeButton = [&controlArea] (juce::TextButton& button,
                                       int width,
                                       int topTrim)
    {
        controlArea.removeFromTop (topTrim);
        auto row = controlArea.removeFromTop (buttonHeight);
        auto x = row.getCentreX() - width / 2;
        button.setBounds (x, row.getY(), width, buttonHeight);
    };

    if (isAutoPageActive)
    {
        placeButton (autoStartButton, 96, 0);
        placeButton (autoStopButton, 96, 5);
        placeButton (autoClearButton, 96, 5);

        controlArea.removeFromTop (16);
        autoSensitivitySlider.setBounds (controlArea.removeFromTop (22).withTrimmedLeft (74));
        controlArea.removeFromTop (4);
        autoMinGapSlider.setBounds (controlArea.removeFromTop (22).withTrimmedLeft (74));

        quarterToneButton.setBounds (rightInner.getCentreX() - 65,
                                     controlArea.getBottom() + 5,
                                     130,
                                     buttonHeight);
        exportMidiButton.setBounds (rightInner.getCentreX() - 56,
                                    quarterToneButton.getBottom() + 5,
                                    112,
                                    buttonHeight);
    }
    else
    {
        placeButton (freezeButton, 96, 0);
        placeButton (unfreezeButton, 112, 5);
        placeButton (resetButton, 84, 5);
        placeButton (quarterToneButton, 130, 5);
        placeButton (exportMidiButton, 112, 5);
    }

    auto peakPanel = getLocalBounds().reduced (12).removeFromRight (206).reduced (12);
    peakPanel.removeFromTop (44);
    peakPanel.removeFromTop (10);
    peakPanel.removeFromBottom (54);
    auto peakTextArea = peakPanel.removeFromBottom (148);
    peakTextArea.removeFromTop (30);
    auto peakInner = peakTextArea.reduced (2, 4);
    peakInner.removeFromTop (22);
    topPeakCountSlider.setBounds (peakInner.removeFromTop (20).withTrimmedLeft (36));

    bassBoostButton.setBounds (scoreArea.getX() + 18,
                               scoreArea.getBottom() - 38,
                               112,
                               buttonHeight);
    bassBoostButton.toFront (false);

    detailAnalysisButton.setBounds (bassBoostButton.getRight() + 10,
                                    scoreArea.getBottom() - 38,
                                    132,
                                    buttonHeight);
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

    const float minDb = -100.0f;
    const float maxDb =   0.0f;

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
            spectrogramImage.setPixelAt (x, y, baseColour.interpolatedWith (juce::Colours::white, 0.92f));
        }

        pendingFreezeMarker = false;
    }
}

//==============================================================================
void AudioPluginAudioProcessorEditor::timerCallback()
{
    processorRef.getSpectrumCopy (latestSpectrum);
    processorRef.getResidualCopy (latestResidual);
    processorRef.getTopPeaksCopy (latestTopFreqs, latestTopMags);

    spectrumForDrawing = latestSpectrum;
    topFreqsForDrawing = latestTopFreqs;
    topMagsForDrawing  = latestTopMags;

    const double nowSeconds = juce::Time::getMillisecondCounterHiRes() * 0.001;
    recordDetailAnalysisFrame (nowSeconds);
    processAutoOnsetDetection (nowSeconds);
    pushSpectrumToImage();

    if (staffComponent != nullptr)
        staffComponent->repaint();

    repaint();
}
