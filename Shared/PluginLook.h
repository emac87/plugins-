#pragma once
// Shared dark look for Contour EQ, Capture Rack and Omnisampler.
// Each plug-in passes its own accent colour.
#include <juce_gui_basics/juce_gui_basics.h>

namespace look
{
    inline const juce::Colour bg        { 0xff15171c };
    inline const juce::Colour panel     { 0xff1d2027 };
    inline const juce::Colour panelHi   { 0xff262a33 };
    inline const juce::Colour outline   { 0xff343945 };
    inline const juce::Colour text      { 0xffdfe3ea };
    inline const juce::Colour textDim   { 0xff8a92a3 };

    class PluginLook : public juce::LookAndFeel_V4
    {
    public:
        explicit PluginLook (juce::Colour accentColour) : accent (accentColour)
        {
            setColour (juce::ResizableWindow::backgroundColourId, bg);
            setColour (juce::Label::textColourId, text);
            setColour (juce::Slider::textBoxTextColourId, text);
            setColour (juce::Slider::textBoxOutlineColourId, juce::Colours::transparentBlack);
            setColour (juce::Slider::textBoxBackgroundColourId, juce::Colours::transparentBlack);
            setColour (juce::Slider::rotarySliderFillColourId, accent);
            setColour (juce::Slider::thumbColourId, accent);
            setColour (juce::Slider::trackColourId, accent.withAlpha (0.6f));
            setColour (juce::Slider::backgroundColourId, outline);
            setColour (juce::ComboBox::backgroundColourId, panelHi);
            setColour (juce::ComboBox::outlineColourId, outline);
            setColour (juce::ComboBox::textColourId, text);
            setColour (juce::ComboBox::arrowColourId, textDim);
            setColour (juce::PopupMenu::backgroundColourId, panel);
            setColour (juce::PopupMenu::textColourId, text);
            setColour (juce::PopupMenu::highlightedBackgroundColourId, accent.withAlpha (0.35f));
            setColour (juce::PopupMenu::highlightedTextColourId, juce::Colours::white);
            setColour (juce::TextButton::buttonColourId, panelHi);
            setColour (juce::TextButton::buttonOnColourId, accent.withAlpha (0.75f));
            setColour (juce::TextButton::textColourOffId, text);
            setColour (juce::TextButton::textColourOnId, juce::Colours::white);
            setColour (juce::ToggleButton::textColourId, text);
            setColour (juce::ToggleButton::tickColourId, accent);
            setColour (juce::ToggleButton::tickDisabledColourId, textDim);
            setColour (juce::TextEditor::backgroundColourId, panelHi);
            setColour (juce::TextEditor::outlineColourId, outline);
            setColour (juce::TextEditor::textColourId, text);
            setColour (juce::TextEditor::focusedOutlineColourId, accent);
            setColour (juce::ListBox::backgroundColourId, panel);
            setColour (juce::ListBox::textColourId, text);
            setColour (juce::ScrollBar::thumbColourId, outline);
            setColour (juce::TabbedButtonBar::tabTextColourId, textDim);
            setColour (juce::TabbedButtonBar::frontTextColourId, text);
            setColour (juce::AlertWindow::backgroundColourId, panel);
            setColour (juce::AlertWindow::textColourId, text);
            setColour (juce::AlertWindow::outlineColourId, outline);
            setColour (juce::DirectoryContentsDisplayComponent::highlightColourId, accent.withAlpha (0.35f));
            setColour (juce::DirectoryContentsDisplayComponent::textColourId, text);
        }

        juce::Colour accent;

        void drawRotarySlider (juce::Graphics& g, int x, int y, int w, int h, float pos,
                               float startAngle, float endAngle, juce::Slider& s) override
        {
            auto bounds = juce::Rectangle<float> ((float) x, (float) y, (float) w, (float) h).reduced (3.0f);
            auto r = juce::jmin (bounds.getWidth(), bounds.getHeight()) * 0.5f;
            auto c = bounds.getCentre();
            auto lineW = juce::jmax (2.0f, r * 0.14f);
            auto arcR = r - lineW * 0.5f;

            juce::Path track;
            track.addCentredArc (c.x, c.y, arcR, arcR, 0.0f, startAngle, endAngle, true);
            g.setColour (outline);
            g.strokePath (track, juce::PathStrokeType (lineW, juce::PathStrokeType::curved, juce::PathStrokeType::rounded));

            // Bipolar sliders (range crosses zero) fill from the middle.
            auto range = s.getRange();
            float fromPos = 0.0f;
            if (range.getStart() < 0.0 && range.getEnd() > 0.0)
                fromPos = (float) s.valueToProportionOfLength (0.0);
            auto a0 = startAngle + fromPos * (endAngle - startAngle);
            auto a1 = startAngle + pos * (endAngle - startAngle);
            juce::Path fill;
            fill.addCentredArc (c.x, c.y, arcR, arcR, 0.0f, juce::jmin (a0, a1), juce::jmax (a0, a1), true);
            auto col = s.isEnabled() ? s.findColour (juce::Slider::rotarySliderFillColourId) : textDim;
            g.setColour (col);
            g.strokePath (fill, juce::PathStrokeType (lineW, juce::PathStrokeType::curved, juce::PathStrokeType::rounded));

            g.setColour (panelHi);
            g.fillEllipse (juce::Rectangle<float> (r * 1.25f, r * 1.25f).withCentre (c));
            juce::Point<float> tip (c.x + std::sin (a1) * r * 0.55f, c.y - std::cos (a1) * r * 0.55f);
            g.setColour (text);
            g.drawLine ({ c.getPointOnCircumference (r * 0.15f, a1), tip }, 2.0f);
        }

        void drawButtonBackground (juce::Graphics& g, juce::Button& b, const juce::Colour& bgCol,
                                   bool over, bool down) override
        {
            auto r = b.getLocalBounds().toFloat().reduced (0.5f);
            auto c = bgCol;
            if (down) c = c.brighter (0.15f);
            else if (over) c = c.brighter (0.07f);
            g.setColour (c);
            g.fillRoundedRectangle (r, 4.0f);
            g.setColour (b.getToggleState() ? accent : outline);
            g.drawRoundedRectangle (r, 4.0f, 1.0f);
        }

        juce::Font getTextButtonFont (juce::TextButton&, int h) override { return juce::Font (juce::FontOptions (juce::jmin (14.0f, h * 0.55f))); }
        juce::Font getComboBoxFont (juce::ComboBox& c) override { return juce::Font (juce::FontOptions (juce::jmin (14.0f, c.getHeight() * 0.55f))); }
        juce::Font getPopupMenuFont() override { return juce::Font (juce::FontOptions (14.0f)); }

        // On iPhone/iPad a plug-in (AUv3 in AUM, Loopy Pro...) can't open windows of its own, so a
        // pop-up menu or combo-box list shown as a separate window never appears. Open it inside
        // the plug-in's own view instead (harmless in the iOS standalone app too).
        juce::Component* getParentComponentForMenuOptions (const juce::PopupMenu::Options& o) override
        {
           #if JUCE_IOS
            if (o.getParentComponent() == nullptr)
                if (auto* t = o.getTargetComponent())
                    return t->getTopLevelComponent();
           #endif
            return juce::LookAndFeel_V4::getParentComponentForMenuOptions (o);
        }

        void drawComboBox (juce::Graphics& g, int w, int h, bool, int, int, int, int, juce::ComboBox& box) override
        {
            auto r = juce::Rectangle<float> (0, 0, (float) w, (float) h).reduced (0.5f);
            g.setColour (box.findColour (juce::ComboBox::backgroundColourId));
            g.fillRoundedRectangle (r, 4.0f);
            g.setColour (box.hasKeyboardFocus (false) ? accent : outline);
            g.drawRoundedRectangle (r, 4.0f, 1.0f);
            juce::Path p;
            auto ax = (float) w - 12.0f, ay = h * 0.5f;
            p.addTriangle (ax - 4, ay - 2, ax + 4, ay - 2, ax, ay + 3);
            g.setColour (textDim);
            g.fillPath (p);
        }
    };

    // A knob with a caption under it. Owns its Slider; attach parameters from outside.
    class Knob : public juce::Component
    {
    public:
        explicit Knob (const juce::String& caption) : label ({}, caption)
        {
            slider.setSliderStyle (juce::Slider::RotaryHorizontalVerticalDrag);
            slider.setTextBoxStyle (juce::Slider::TextBoxBelow, false, 70, 16);
            label.setJustificationType (juce::Justification::centred);
            label.setFont (juce::FontOptions (12.0f));
            label.setColour (juce::Label::textColourId, textDim);
            addAndMakeVisible (slider);
            addAndMakeVisible (label);
        }
        void resized() override
        {
            auto r = getLocalBounds();
            label.setBounds (r.removeFromTop (16));
            slider.setBounds (r);
        }
        juce::Slider slider;
        juce::Label label;
    };

    inline void drawPanel (juce::Graphics& g, juce::Rectangle<float> r, float corner = 6.0f)
    {
        g.setColour (panel);
        g.fillRoundedRectangle (r, corner);
        g.setColour (outline);
        g.drawRoundedRectangle (r.reduced (0.5f), corner, 1.0f);
    }

    // ---- iPhone/iPad: a plug-in (AUv3) can't open windows of its own -------------------------
    // Parent for a pop-up menu shown at a screen position (no target component): the plug-in's
    // own view on iOS, a normal window elsewhere.
    inline juce::Component* menuParent (juce::Component* c)
    {
       #if JUCE_IOS
        return c != nullptr ? c->getTopLevelComponent() : nullptr;
       #else
        juce::ignoreUnused (c);
        return nullptr;
       #endif
    }

    // Options for a pop-up menu under `target`. On iOS the menu is put inside the plug-in's view
    // explicitly: a menu that hasn't been given a LookAndFeel never asks PluginLook where to go,
    // so without this it opens as its own window, which an AUv3 (AUM etc.) never shows.
    inline juce::PopupMenu::Options menuFor (juce::Component* target)
    {
        return juce::PopupMenu::Options().withTargetComponent (target).withParentComponent (menuParent (target));
    }

    // Shows an AlertWindow modally (it deletes itself). On iOS it goes inside the plug-in's view.
    inline void presentAlert (juce::Component* owner, juce::AlertWindow* w, std::function<void (int)> done = {})
    {
       #if JUCE_IOS
        if (auto* top = owner != nullptr ? owner->getTopLevelComponent() : nullptr)
        {
            w->removeFromDesktop();
            top->addAndMakeVisible (w);
            w->setCentrePosition (top->getLocalBounds().getCentre());
        }
       #else
        juce::ignoreUnused (owner);
       #endif
        w->enterModalState (true, juce::ModalCallbackFunction::create ([done] (int r) { if (done) done (r); }), true);
    }

    inline void showMessage (juce::Component* owner, const juce::String& title, const juce::String& text,
                             juce::MessageBoxIconType icon = juce::MessageBoxIconType::InfoIcon)
    {
       #if JUCE_IOS
        auto* w = new juce::AlertWindow (title, text, icon, owner);
        w->addButton ("OK", 0, juce::KeyPress (juce::KeyPress::returnKey));
        presentAlert (owner, w);
       #else
        juce::AlertWindow::showMessageBoxAsync (icon, title, text, "OK", owner);
       #endif
    }
}
