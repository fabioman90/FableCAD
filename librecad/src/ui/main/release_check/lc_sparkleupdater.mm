#include "lc_sparkleupdater.h"

#import <Foundation/Foundation.h>
#import <Sparkle/Sparkle.h>

bool LC_SparkleUpdater::check(bool manual, bool automatic) {
    auto* bundle = [NSBundle mainBundle];
    id url = [bundle objectForInfoDictionaryKey:@"SUFeedURL"];
    id publicKey = [bundle objectForInfoDictionaryKey:@"SUPublicEDKey"];
    if (![url isKindOfClass:[NSString class]] || ![publicKey isKindOfClass:[NSString class]] ||
        ![url hasPrefix:@"https://"] || [publicKey length] == 0) {
        return false;
    }

    static SPUStandardUpdaterController* controller =
        [[SPUStandardUpdaterController alloc] initWithStartingUpdater:YES
                                                       updaterDelegate:nil
                                                    userDriverDelegate:nil];
    if (controller.updater.automaticallyChecksForUpdates != automatic)
        controller.updater.automaticallyChecksForUpdates = automatic;
    if (manual)
        [controller checkForUpdates:nil];
    return true;
}
