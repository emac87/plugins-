#include "IOSFiles.h"

#if JUCE_IOS
#import <UIKit/UIKit.h>
#import <UniformTypeIdentifiers/UniformTypeIdentifiers.h>
#include <objc/runtime.h>

#if __has_feature(objc_arc)
 #define OMNI_RELEASE(x)
#else
 #define OMNI_RELEASE(x) [x release]
#endif

using ImportCallback = std::function<void (juce::Array<juce::File>)>;

static NSString* toNS (const juce::String& s)   { return [NSString stringWithUTF8String: s.toRawUTF8()]; }
static juce::String fromNS (NSString* s)        { return s != nil ? juce::String::fromUTF8 ([s UTF8String]) : juce::String(); }

static juce::File importedDir()
{
    auto dir = juce::File::getSpecialLocation (juce::File::userDocumentsDirectory).getChildFile ("Imported");
    dir.createDirectory();
    return dir;
}

// Copies one picked item into Documents/Imported and returns the copy (or an invalid File).
static juce::File importItem (NSURL* url, bool isFolder)
{
    NSFileManager* fm = [NSFileManager defaultManager];
    const auto name = juce::File::createLegalFileName (fromNS ([url lastPathComponent]));
    if (name.isEmpty()) return {};

    juce::File dest;
    if (isFolder)
    {
        dest = importedDir().getChildFile (name);
        dest.deleteRecursively();   // re-importing a folder replaces the old copy
    }
    else
    {
        auto samples = importedDir().getChildFile ("Files");
        samples.createDirectory();
        dest = samples.getChildFile (name);
        NSNumber* size = nil;
        [url getResourceValue: &size forKey: NSURLFileSizeKey error: nil];
        // Same name and size: the file is already here. Same name, different file: keep both.
        if (dest.existsAsFile() && size != nil && dest.getSize() == (juce::int64) [size longLongValue]) return dest;
        dest = dest.getNonexistentSibling (false);
    }

    NSURL* destUrl = [NSURL fileURLWithPath: toNS (dest.getFullPathName())];
    __block BOOL ok = NO;
    NSError* coordError = nil;
    NSFileCoordinator* coordinator = [[NSFileCoordinator alloc] initWithFilePresenter: nil];
    [coordinator coordinateReadingItemAtURL: url
                                    options: NSFileCoordinatorReadingWithoutChanges
                                      error: &coordError
                                 byAccessor: ^(NSURL* readable)
    {
        NSError* copyError = nil;
        ok = [fm copyItemAtURL: readable toURL: destUrl error: &copyError];
    }];
    OMNI_RELEASE (coordinator);
    return ok ? dest : juce::File();
}

@interface OmniPickerDelegate : NSObject <UIDocumentPickerDelegate>
{
@public
    ImportCallback done;
    bool folder;
}
@end

@implementation OmniPickerDelegate

- (void) documentPicker: (UIDocumentPickerViewController*) controller didPickDocumentsAtURLs: (NSArray<NSURL*>*) urls
{
    juce::ignoreUnused (controller);
    const ImportCallback callback = done;
    const bool isFolder = folder;
    NSArray<NSURL*>* picked = [urls copy];

    // Copying a big library takes a while, so it runs off the main thread.
    dispatch_async (dispatch_get_global_queue (QOS_CLASS_USER_INITIATED, 0), ^
    {
        juce::Array<juce::File> imported;
        for (NSURL* url in picked)
        {
            const BOOL scoped = [url startAccessingSecurityScopedResource];
            const auto f = importItem (url, isFolder);
            if (f != juce::File()) imported.add (f);
            if (scoped) [url stopAccessingSecurityScopedResource];
        }
        OMNI_RELEASE (picked);
        const juce::Array<juce::File> result (imported);
        dispatch_async (dispatch_get_main_queue(), ^
        {
            if (callback) callback (result);
        });
    });
}

@end

static UIViewController* viewControllerFor (juce::Component& c)
{
    auto* peer = c.getPeer();
    if (peer == nullptr) return nil;
    UIView* view = (__bridge UIView*) peer->getNativeHandle();
    UIViewController* vc = nil;
    for (UIResponder* r = view; r != nil; r = [r nextResponder])
        if ([r isKindOfClass: [UIViewController class]]) { vc = (UIViewController*) r; break; }
    if (vc == nil) vc = [[view window] rootViewController];
    while (vc != nil && [vc presentedViewController] != nil) vc = [vc presentedViewController];
    return vc;
}

namespace omni::ios
{
    void pickAndImport (juce::Component& anchor, Pick what, bool multiple, ImportCallback done)
    {
        UIViewController* parent = viewControllerFor (anchor);
        if (parent == nil) return;

        const bool folder = what == Pick::folder;
        // Files come in as copies (asCopy), so any file type can be picked and read.
        // Folders can't be copied by the picker, so they are opened in place and copied by importItem.
        UIDocumentPickerViewController* picker = folder
            ? [[UIDocumentPickerViewController alloc] initForOpeningContentTypes: @[ UTTypeFolder ]]
            : [[UIDocumentPickerViewController alloc] initForOpeningContentTypes: @[ UTTypeItem ] asCopy: YES];
        [picker setAllowsMultipleSelection: (multiple && ! folder) ? YES : NO];

        OmniPickerDelegate* delegate = [[OmniPickerDelegate alloc] init];
        delegate->done = std::move (done);
        delegate->folder = folder;
        [picker setDelegate: delegate];
        // The picker doesn't keep its delegate alive, so tie the delegate's lifetime to the picker.
        static char delegateKey;
        objc_setAssociatedObject (picker, &delegateKey, delegate, OBJC_ASSOCIATION_RETAIN_NONATOMIC);
        OMNI_RELEASE (delegate);

        [parent presentViewController: picker animated: YES completion: nil];
        OMNI_RELEASE (picker);
    }
}
#endif
