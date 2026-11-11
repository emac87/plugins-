#pragma once
// The capture library: category folders on disk plus a small metadata file
// (favourites, author, source) at the library root.
#include <juce_core/juce_core.h>

namespace rack
{
    struct LibraryItem
    {
        juce::File file;
        juce::String category, author, source, license, toneUrl;
        bool favourite = false;
        bool isIR() const { return file.hasFileExtension ("wav;aif;aiff"); }
    };

    class Library
    {
    public:
        static inline const char* defaultCategories[] = { "Amps", "Amp + Cab", "Pedals", "Outboard", "Bass", "IRs", "Space", "Experimental" };

        Library() { setRoot (defaultRoot()); }

        static juce::File defaultRoot()
        {
            return juce::File::getSpecialLocation (juce::File::userDocumentsDirectory).getChildFile ("Capture Rack/Library");
        }

        void setRoot (const juce::File& r)
        {
            root = r;
            root.createDirectory();
            for (auto* c : defaultCategories) root.getChildFile (c).createDirectory();
            loadMeta();
        }
        const juce::File& getRoot() const { return root; }

        juce::StringArray categories() const
        {
            juce::StringArray out;
            for (auto& d : root.findChildFiles (juce::File::findDirectories, false))
                if (! d.getFileName().startsWithChar ('.')) out.add (d.getFileName());
            out.sort (true);
            return out;
        }

        juce::File addCategory (const juce::String& name)
        {
            auto d = root.getChildFile (juce::File::createLegalFileName (name.trim()));
            d.createDirectory();
            return d;
        }

        // All captures in a category ("" = everything, "Favourites" = starred), filtered by text.
        juce::Array<LibraryItem> items (const juce::String& category, const juce::String& filter) const
        {
            juce::Array<LibraryItem> out;
            const bool favs = category == "Favourites";
            const auto dir = (category.isEmpty() || favs) ? root : root.getChildFile (category);
            for (auto& f : dir.findChildFiles (juce::File::findFiles, true, "*.nam;*.wav;*.aif;*.aiff"))
            {
                auto it = describe (f);
                if (favs && ! it.favourite) continue;
                if (filter.isNotEmpty() && ! (f.getFileName().containsIgnoreCase (filter) || it.author.containsIgnoreCase (filter))) continue;
                out.add (it);
            }
            std::sort (out.begin(), out.end(), [] (const LibraryItem& a, const LibraryItem& b)
                       { return a.file.getFileName().compareIgnoreCase (b.file.getFileName()) < 0; });
            return out;
        }

        LibraryItem describe (const juce::File& f) const
        {
            LibraryItem it;
            it.file = f;
            it.category = f.getRelativePathFrom (root).upToFirstOccurrenceOf ("/", false, false).upToFirstOccurrenceOf ("\\", false, false);
            if (auto* o = meta.getProperty (key (f), {}).getDynamicObject())
            {
                it.favourite = o->getProperty ("favourite");
                it.author = o->getProperty ("author").toString();
                it.source = o->getProperty ("source").toString();
                it.license = o->getProperty ("license").toString();
                it.toneUrl = o->getProperty ("toneUrl").toString();
            }
            return it;
        }

        void setFavourite (const juce::File& f, bool fav) { entry (f)->setProperty ("favourite", fav); saveMeta(); }

        void setInfo (const juce::File& f, const juce::String& author, const juce::String& source, const juce::String& license, const juce::String& url)
        {
            auto* o = entry (f);
            o->setProperty ("author", author);
            o->setProperty ("source", source);
            o->setProperty ("license", license);
            o->setProperty ("toneUrl", url);
            saveMeta();
        }

        // Copies files into a category. Returns the copies.
        juce::Array<juce::File> import (const juce::Array<juce::File>& files, const juce::String& category)
        {
            juce::Array<juce::File> out;
            auto dir = addCategory (category.isNotEmpty() ? category : juce::String ("Amps"));
            for (auto& f : files)
            {
                if (! f.existsAsFile()) continue;
                auto dest = dir.getNonexistentChildFile (f.getFileNameWithoutExtension(), f.getFileExtension(), false);
                if (f.getParentDirectory() == dir) dest = f;
                else if (! f.copyFileTo (dest)) continue;
                out.add (dest);
            }
            return out;
        }

    private:
        juce::String key (const juce::File& f) const { return f.getRelativePathFrom (root).replaceCharacter ('\\', '/'); }

        juce::DynamicObject* entry (const juce::File& f)
        {
            auto k = key (f);
            if (! meta.getProperty (k, {}).isObject())
                meta.getDynamicObject()->setProperty (k, juce::var (new juce::DynamicObject()));
            return meta.getProperty (k, {}).getDynamicObject();
        }

        void loadMeta()
        {
            meta = juce::JSON::parse (root.getChildFile ("library.json"));
            if (! meta.isObject()) meta = juce::var (new juce::DynamicObject());
        }
        void saveMeta() { root.getChildFile ("library.json").replaceWithText (juce::JSON::toString (meta)); }

        juce::File root;
        juce::var meta;
    };
}
