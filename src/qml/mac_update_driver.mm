#include "qml/mac_update_driver.h"

#include <QCoreApplication>
#include <algorithm>
#include <utility>

#import <Sparkle/Sparkle.h>

@interface LumenSparkleDriver : NSObject <SPUUserDriver, SPUUpdaterDelegate> {
  @public
    NSString* feed;
    NSString* expectedVersion;
    void (^cancelDownload)(void);
    void (^installUpdate)(SPUUserUpdateChoice);
    std::function<void(int)> reportProgress;
    std::function<void(const QString&)> reportState;
    std::function<void(const QString&)> reportError;
    uint64_t expectedBytes;
    uint64_t receivedBytes;
    bool active;
    bool installing;
    QString rejectedUpdate;
}
@end

@implementation LumenSparkleDriver

- (NSString*)feedURLStringForUpdater:(SPUUpdater*)updater {
    return feed;
}

- (void)showUpdatePermissionRequest:(SPUUpdatePermissionRequest*)request
                              reply:(void (^)(SUUpdatePermissionResponse*))reply {
    // Lumen owns the schedule and asks for consent in its update dialog.
    reply([[SUUpdatePermissionResponse alloc] initWithAutomaticUpdateChecks:NO sendSystemProfile:NO]);
}

- (void)showUserInitiatedUpdateCheckWithCancellation:(void (^)(void))cancellation {
    cancelDownload = [cancellation copy];
}

- (void)showUpdateFoundWithAppcastItem:(SUAppcastItem*)item
                                state:(SPUUserUpdateState*)state
                                reply:(void (^)(SPUUserUpdateChoice))reply {
    cancelDownload = nil;
    if (!active || item.informationOnlyUpdate ||
        ![item.displayVersionString isEqualToString:expectedVersion]) {
        rejectedUpdate = QCoreApplication::translate("UpdateErrors", "The update feed does not match this release. Check for updates again.");
        reply(SPUUserUpdateChoiceDismiss);
        return;
    }
    if (state.stage == SPUUserUpdateStageInstalling) {
        [self showReadyToInstallAndRelaunch:reply];
        return;
    }
    reply(SPUUserUpdateChoiceInstall);
}

- (void)showUpdateReleaseNotesWithDownloadData:(SPUDownloadData*)data {}
- (void)showUpdateReleaseNotesFailedToDownloadWithError:(NSError*)error {}

- (void)showUpdateNotFoundWithError:(NSError*)error acknowledgement:(void (^)(void))acknowledgement {
    acknowledgement();
}

- (void)showUpdaterError:(NSError*)error acknowledgement:(void (^)(void))acknowledgement {
    acknowledgement();
}

- (void)showDownloadInitiatedWithCancellation:(void (^)(void))cancellation {
    cancelDownload = [cancellation copy];
    reportState(QStringLiteral("downloading"));
}

- (void)showDownloadDidReceiveExpectedContentLength:(uint64_t)length {
    expectedBytes = length;
}

- (void)showDownloadDidReceiveDataOfLength:(uint64_t)length {
    receivedBytes += length;
    if (expectedBytes > 0) {
        const double fraction = static_cast<double>(receivedBytes) / static_cast<double>(expectedBytes);
        reportProgress(static_cast<int>(std::min(1.0, fraction) * 100.0));
    }
}

- (void)showDownloadDidStartExtractingUpdate {
    cancelDownload = nil;
    reportState(QStringLiteral("verifying"));
}

- (void)showExtractionReceivedProgress:(double)progress {}

- (void)showReadyToInstallAndRelaunch:(void (^)(SPUUserUpdateChoice))reply {
    installUpdate = [reply copy];
    reportState(QStringLiteral("ready"));
}

- (void)showInstallingUpdateWithApplicationTerminated:(BOOL)terminated
                        retryTerminatingApplication:(void (^)(void))retry {
    reportState(QStringLiteral("installing"));
}

- (void)showUpdateInstalledAndRelaunched:(BOOL)relaunched
                        acknowledgement:(void (^)(void))acknowledgement {
    acknowledgement();
}

- (void)dismissUpdateInstallation {
    cancelDownload = nil;
    installUpdate = nil;
}

- (void)updater:(SPUUpdater*)updater didFinishUpdateCycleForUpdateCheck:(SPUUpdateCheck)check
          error:(NSError*)error {
    if (!std::exchange(active, false)) return;
    if (installing && !error) return;
    if (!rejectedUpdate.isEmpty()) reportError(rejectedUpdate);
    else if (error) reportError(QString::fromNSString(error.localizedDescription));
    else reportError(QCoreApplication::translate("UpdateErrors", "The update did not finish. Try again."));
}

@end

namespace mvpview {

struct MacUpdateDriver::Impl {
    LumenSparkleDriver* driver = [LumenSparkleDriver new];
    SPUUpdater* updater = nil;
};

MacUpdateDriver::MacUpdateDriver(std::function<void(int)> progress,
                                 std::function<void(const QString&)> state,
                                 std::function<void(const QString&)> error)
    : impl_(std::make_unique<Impl>()) {
    impl_->driver->reportProgress = std::move(progress);
    impl_->driver->reportState = std::move(state);
    impl_->driver->reportError = std::move(error);
}

MacUpdateDriver::~MacUpdateDriver() = default;

bool MacUpdateDriver::isConfigured() {
    NSString* key = [NSBundle.mainBundle objectForInfoDictionaryKey:@"SUPublicEDKey"];
    return key.length > 0;
}

void MacUpdateDriver::start(const QUrl& appcast, const QString& version) {
    auto* driver = impl_->driver;
    driver->feed = appcast.toString(QUrl::FullyEncoded).toNSString();
    driver->expectedVersion = version.toNSString();
    driver->receivedBytes = 0;
    driver->expectedBytes = 0;
    driver->installing = false;
    driver->rejectedUpdate.clear();
    if (!impl_->updater) {
        SPUUpdater* updater = [[SPUUpdater alloc] initWithHostBundle:NSBundle.mainBundle
                                                  applicationBundle:NSBundle.mainBundle
                                                         userDriver:driver delegate:driver];
        NSError* error = nil;
        if (![updater startUpdater:&error]) {
            driver->reportError(QString::fromNSString(error.localizedDescription));
            return;
        }
        impl_->updater = updater;
    }
    if (impl_->updater.sessionInProgress) {
        driver->reportError(QCoreApplication::translate("UpdateErrors", "An update is already in progress."));
        return;
    }
    driver->active = true;
    [impl_->updater checkForUpdates];
}

void MacUpdateDriver::cancel() {
    auto cancellation = impl_->driver->cancelDownload;
    if (cancellation) cancellation();
}

void MacUpdateDriver::install() {
    auto* driver = impl_->driver;
    auto reply = driver->installUpdate;
    if (!reply) {
        driver->reportError(QCoreApplication::translate("UpdateErrors", "The update did not finish. Try again."));
        return;
    }
    driver->installUpdate = nil;
    driver->installing = true;
    reply(SPUUserUpdateChoiceInstall);
}

} // namespace mvpview
