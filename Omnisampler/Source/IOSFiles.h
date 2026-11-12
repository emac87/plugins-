#pragma once
// iPhone/iPad file loading. An iOS app can only keep reading files inside its own sandbox, so
// whatever the user picks in the system file picker is copied into the app's Documents/Imported
// folder first, and the copies are what gets loaded (and what saved kits and sessions point at).
#include <juce_gui_basics/juce_gui_basics.h>

#if JUCE_IOS
namespace omni::ios
{
    enum class Pick { files, folder };

    // Shows the system file picker above `anchor`. `done` is called on the message thread with the
    // imported copies (a picked folder comes back as one directory). Not called if the user cancels.
    void pickAndImport (juce::Component& anchor, Pick what, bool multiple,
                        std::function<void (juce::Array<juce::File>)> done);
}
#endif
