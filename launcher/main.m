#import <Cocoa/Cocoa.h>
#import <CommonCrypto/CommonDigest.h>
#include "image.h"

static void alert(NSString *text){
 NSAlert *a=[NSAlert new];a.messageText=@"Gearhead Garage";a.informativeText=text;[a addButtonWithTitle:@"OK"];[a runModal];
}
static NSTextField *label(NSString *text){return [NSTextField labelWithString:text];}
static NSTextField *note(NSString *text){NSTextField *t=label(text);t.font=[NSFont systemFontOfSize:11];t.textColor=NSColor.secondaryLabelColor;return t;}
static NSButton *check(NSString *title,BOOL enabled){
 NSButton *b=[NSButton checkboxWithTitle:title target:nil action:nil];b.state=enabled?NSControlStateValueOn:NSControlStateValueOff;return b;
}
// Updates are Builder releases: the new Builder makes the new app from the game inside this one.
static NSString *const ReleasesURL=@"https://api.github.com/repos/007hacky007/gearhead-garage-macos-arm64/releases/latest";
static NSString *const BuilderAsset=@"Gearhead-Garage-Builder.dmg";
static NSString *const BuilderName=@"Gearhead Garage Builder.app";
static NSString *const BuilderID=@"org.classicgames.gearheadgarage.builder";
static NSString *stripV(NSString *tag){return [tag hasPrefix:@"v"]?[tag substringFromIndex:1]:tag;}
static BOOL newer(NSString *candidate,NSString *current){
 NSCharacterSet *plain=[NSCharacterSet characterSetWithCharactersInString:@"0123456789."];
 if(!candidate.length||[candidate rangeOfCharacterFromSet:plain.invertedSet].location!=NSNotFound)return NO;
 return [candidate compare:current options:NSNumericSearch]==NSOrderedDescending;
}
static NSString *fileSHA256(NSURL *url){
 NSData *data=[NSData dataWithContentsOfURL:url options:NSDataReadingMappedIfSafe error:nil];if(!data)return nil;
 unsigned char digest[CC_SHA256_DIGEST_LENGTH];CC_SHA256(data.bytes,(CC_LONG)data.length,digest);
 NSMutableString *hash=[NSMutableString new];for(unsigned i=0;i<sizeof digest;i++)[hash appendFormat:@"%02x",digest[i]];return hash;
}
static NSString *runTool(NSString *path,NSArray *arguments){
 NSTask *task=[NSTask new];task.executableURL=[NSURL fileURLWithPath:path];task.arguments=arguments;
 NSPipe *pipe=[NSPipe pipe];task.standardOutput=pipe;task.standardError=pipe;NSError *error=nil;
 if(![task launchAndReturnError:&error])return error.localizedDescription;
 NSData *output=[pipe.fileHandleForReading readDataToEndOfFile];[task waitUntilExit];
 return task.terminationStatus?[[NSString alloc] initWithData:output encoding:NSUTF8StringEncoding]?:@"failed":nil;
}
static NSError *failure(NSString *text){return [NSError errorWithDomain:@"Gearhead" code:1 userInfo:@{NSLocalizedDescriptionKey:text}];}

@interface Launcher : NSObject <NSApplicationDelegate,NSWindowDelegate>
@property NSWindow *window;
@property NSPopUpButton *graphics;
@property NSButton *fullscreen,*sound,*playButton,*updateButton,*autoCheck;
@property NSTextField *status;
@property NSURL *resources,*support,*saves,*logs;
@property NSTask *game;
@property NSDictionary *latestRelease;
@property NSURLSessionDownloadTask *download;
@property NSTimer *progressTimer;
@end
@implementation Launcher
- (NSURL *)url:(NSString *)path{return [self.resources URLByAppendingPathComponent:path];}
- (BOOL)validGame{
 NSData *data=[NSData dataWithContentsOfURL:[self url:@"linked.exe"] options:NSDataReadingMappedIfSafe error:nil];
 if(data.length!=IMAGE_FILE_SIZE)return NO;
 unsigned char digest[CC_SHA256_DIGEST_LENGTH];CC_SHA256(data.bytes,(CC_LONG)data.length,digest);
 NSMutableString *hash=[NSMutableString new];for(unsigned i=0;i<sizeof digest;i++)[hash appendFormat:@"%02x",digest[i]];
 return [hash isEqualToString:@IMAGE_SHA256]&&[NSFileManager.defaultManager fileExistsAtPath:[self url:@"Game/Data/Gfx24.dat"].path];
}
- (void)applicationDidFinishLaunching:(NSNotification *)notification{
 (void)notification;self.resources=NSBundle.mainBundle.resourceURL;
 if(![self validGame]){alert(@"The game files are missing or do not match this build. Build the app again with ./build.sh and your own Gearhead Garage disc.");[NSApp terminate:nil];return;}
 NSFileManager *files=NSFileManager.defaultManager;NSError *error=nil;
 self.support=[[files URLsForDirectory:NSApplicationSupportDirectory inDomains:NSUserDomainMask].firstObject URLByAppendingPathComponent:@"Gearhead Garage" isDirectory:YES];
 self.saves=[self.support URLByAppendingPathComponent:@"Saves" isDirectory:YES];
 self.logs=[self.support URLByAppendingPathComponent:@"Logs" isDirectory:YES];
 for(NSURL *folder in @[self.saves,self.logs])
  if(![files createDirectoryAtURL:folder withIntermediateDirectories:YES attributes:nil error:&error]){alert(error.localizedDescription);[NSApp terminate:nil];return;}
 NSUserDefaults *settings=NSUserDefaults.standardUserDefaults;
 [settings registerDefaults:@{@"Graphics":@"d3d",@"Fullscreen":@NO,@"Sound":@YES,@"CheckForUpdates":@YES}];
 self.window=[[NSWindow alloc] initWithContentRect:NSMakeRect(0,0,460,360) styleMask:NSWindowStyleMaskTitled|NSWindowStyleMaskClosable|NSWindowStyleMaskMiniaturizable backing:NSBackingStoreBuffered defer:NO];
 self.window.title=@"Gearhead Garage";self.window.delegate=self;self.window.releasedWhenClosed=NO;
 NSStackView *stack=[NSStackView new];stack.orientation=NSUserInterfaceLayoutOrientationVertical;stack.alignment=NSLayoutAttributeLeading;stack.spacing=12;stack.translatesAutoresizingMaskIntoConstraints=NO;
 [self.window.contentView addSubview:stack];
 [NSLayoutConstraint activateConstraints:@[[stack.leadingAnchor constraintEqualToAnchor:self.window.contentView.leadingAnchor constant:22],[stack.trailingAnchor constraintEqualToAnchor:self.window.contentView.trailingAnchor constant:-22],[stack.topAnchor constraintEqualToAnchor:self.window.contentView.topAnchor constant:22],[stack.bottomAnchor constraintLessThanOrEqualToAnchor:self.window.contentView.bottomAnchor constant:-18]]];
 NSTextField *title=label(@"Gearhead Garage");title.font=[NSFont systemFontOfSize:24 weight:NSFontWeightSemibold];[stack addArrangedSubview:title];
 [stack addArrangedSubview:note([NSString stringWithFormat:@"Snap-on Gearhead Garage, Build 131 - Mac version %@",self.version])];
 self.graphics=[[NSPopUpButton alloc] initWithFrame:NSZeroRect pullsDown:NO];
 [self.graphics addItemsWithTitles:@[@"Direct3D (full color)",@"Software (256 colors)"]];
 [self.graphics selectItemAtIndex:[[settings stringForKey:@"Graphics"] isEqualToString:@"software"]?1:0];
 NSGridView *grid=[NSGridView gridViewWithViews:@[@[label(@"Graphics"),self.graphics]]];grid.columnSpacing=18;[stack addArrangedSubview:grid];
 self.fullscreen=check(@"Fullscreen (Control-Command-F or F11 switches while playing)",[settings boolForKey:@"Fullscreen"]);
 self.sound=check(@"Sound",[settings boolForKey:@"Sound"]);
 [stack addArrangedSubview:self.fullscreen];[stack addArrangedSubview:self.sound];
 [stack addArrangedSubview:note(@"The game runs at its original 640x480, scaled to the window with the\noriginal 4:3 picture. Your mechanics and snapshots stay in Saves.")];
 self.autoCheck=check(@"Check for updates when the launcher opens",[settings boolForKey:@"CheckForUpdates"]);
 self.autoCheck.target=self;self.autoCheck.action=@selector(autoCheckChanged:);[stack addArrangedSubview:self.autoCheck];
 self.status=label(@"Ready");self.status.font=[NSFont systemFontOfSize:11];
 self.updateButton=[NSButton buttonWithTitle:@"Update" target:self action:@selector(update:)];self.updateButton.controlSize=NSControlSizeSmall;
 self.updateButton.font=[NSFont systemFontOfSize:11];self.updateButton.hidden=YES;
 NSStackView *statusRow=[NSStackView stackViewWithViews:@[self.status,self.updateButton]];statusRow.spacing=8;[stack addArrangedSubview:statusRow];
 self.playButton=[NSButton buttonWithTitle:@"Play" target:self action:@selector(play:)];self.playButton.keyEquivalent=@"\r";
 NSButton *saves=[NSButton buttonWithTitle:@"Open Saves" target:self action:@selector(openSaves:)];
 NSButton *logs=[NSButton buttonWithTitle:@"Open Logs" target:self action:@selector(openLogs:)];
 NSButton *help=[NSButton buttonWithTitle:@"Game Help" target:self action:@selector(openHelp:)];
 NSStackView *buttons=[NSStackView stackViewWithViews:@[self.playButton,saves,logs,help]];buttons.spacing=10;[stack addArrangedSubview:buttons];
 NSMenu *menu=[NSMenu new];NSMenuItem *app=[NSMenuItem new];[menu addItem:app];NSMenu *appMenu=[NSMenu new];app.submenu=appMenu;
 [appMenu addItemWithTitle:@"Check for Updates..." action:@selector(checkForUpdates:) keyEquivalent:@""].target=self;
 [appMenu addItem:NSMenuItem.separatorItem];
 [appMenu addItemWithTitle:@"Quit Gearhead Garage" action:@selector(terminate:) keyEquivalent:@"q"];NSApp.mainMenu=menu;
 [self.window setContentSize:NSMakeSize(460,stack.fittingSize.height+40)];[self.window center];[self.window makeKeyAndOrderFront:nil];[NSApp activateIgnoringOtherApps:YES];
 if([NSProcessInfo.processInfo.arguments containsObject:@"--play"])[self play:nil];
 else if([settings boolForKey:@"CheckForUpdates"])[self checkForUpdates:nil];
}
- (NSString *)version{return NSBundle.mainBundle.infoDictionary[@"CFBundleShortVersionString"]?:@"0";}
- (void)autoCheckChanged:(id)sender{(void)sender;[NSUserDefaults.standardUserDefaults setBool:self.autoCheck.state==NSControlStateValueOn forKey:@"CheckForUpdates"];}
- (NSURL *)assetURL:(NSString *)name{
 for(NSDictionary *asset in self.latestRelease[@"assets"])
  if([asset isKindOfClass:NSDictionary.class]&&[asset[@"name"] isEqual:name]&&[asset[@"browser_download_url"] isKindOfClass:NSString.class])
   return [NSURL URLWithString:asset[@"browser_download_url"]];
 return nil;
}
- (NSMutableURLRequest *)request:(NSURL *)url{
 NSMutableURLRequest *request=[NSMutableURLRequest requestWithURL:url];request.timeoutInterval=30;
 [request setValue:[@"GearheadGarageLauncher/" stringByAppendingString:self.version] forHTTPHeaderField:@"User-Agent"];return request;
}
// sender nil: the quiet check at launch, which says nothing unless there is an update.
- (void)checkForUpdates:(id)sender{
 BOOL manual=sender!=nil;if(self.download)return;
 NSMutableURLRequest *request=[self request:[NSURL URLWithString:ReleasesURL]];[request setValue:@"application/vnd.github+json" forHTTPHeaderField:@"Accept"];
 if(manual)self.status.stringValue=@"Checking for updates...";
 [[NSURLSession.sharedSession dataTaskWithRequest:request completionHandler:^(NSData *data,NSURLResponse *response,NSError *error){
  NSDictionary *release=nil;NSString *problem=error.localizedDescription;
  if(!error&&((NSHTTPURLResponse *)response).statusCode!=200)problem=[NSString stringWithFormat:@"GitHub answered with HTTP %ld",(long)((NSHTTPURLResponse *)response).statusCode];
  else if(!error){release=[NSJSONSerialization JSONObjectWithData:data options:0 error:nil];if(![release isKindOfClass:NSDictionary.class]||![release[@"tag_name"] isKindOfClass:NSString.class]){release=nil;problem=@"GitHub sent an unexpected answer";}}
  dispatch_async(dispatch_get_main_queue(),^{[self foundRelease:release problem:problem manual:manual];});
 }] resume];
}
- (void)foundRelease:(NSDictionary *)release problem:(NSString *)problem manual:(BOOL)manual{
 if(self.download)return;
 if(manual&&!self.game)self.status.stringValue=@"Ready";
 if(!release){if(manual)alert([@"Could not check for updates: " stringByAppendingString:problem]);return;}
 NSString *latest=stripV(release[@"tag_name"]);self.latestRelease=release;
 if(!newer(latest,self.version)||![self assetURL:BuilderAsset]||![self assetURL:[BuilderAsset stringByAppendingString:@".sha256"]]){
  self.latestRelease=nil;if(manual)alert([NSString stringWithFormat:@"You have the latest version (%@).",self.version]);return;
 }
 if(!self.game)self.status.stringValue=[NSString stringWithFormat:@"Version %@ is available",latest];
 self.updateButton.title=[@"Update to " stringByAppendingString:latest];self.updateButton.hidden=NO;
 if(manual)[self update:nil];
}
- (void)updateFailed:(NSString *)text{
 [self.progressTimer invalidate];self.progressTimer=nil;self.download=nil;
 self.status.stringValue=@"Update failed";self.updateButton.enabled=YES;self.playButton.enabled=self.game==nil;alert(text);
}
- (void)update:(id)sender{
 (void)sender;if(self.game||self.download||!self.latestRelease)return;
 NSString *latest=stripV(self.latestRelease[@"tag_name"]);
 NSURL *app=NSBundle.mainBundle.bundleURL,*folder=app.URLByDeletingLastPathComponent;
 if(![app.lastPathComponent isEqualToString:@"Gearhead Garage.app"]){alert(@"Rename this app back to Gearhead Garage.app first: the update replaces it by that name.");return;}
 if(![NSFileManager.defaultManager isWritableFileAtPath:folder.path]){alert([NSString stringWithFormat:@"You cannot write to %@, so the app there cannot be replaced.",folder.path]);return;}
 if([NSRunningApplication runningApplicationsWithBundleIdentifier:BuilderID].count){alert(@"Quit Gearhead Garage Builder first, then click Update again.");return;}
 NSString *notes=[self.latestRelease[@"body"] isKindOfClass:NSString.class]?self.latestRelease[@"body"]:@"";
 if(notes.length>1200)notes=[[notes substringToIndex:1200] stringByAppendingString:@"..."];
 NSAlert *ask=[NSAlert new];ask.messageText=[NSString stringWithFormat:@"Update to Gearhead Garage %@?",latest];
 ask.informativeText=[NSString stringWithFormat:@"The launcher downloads Gearhead Garage Builder %@ and quits. The Builder then makes the new app from the game inside this one, which takes a few minutes, and opens it. Your mechanics and snapshots stay.%@%@",latest,notes.length?@"\n\nWhat's new:\n":@"",notes];
 [ask addButtonWithTitle:@"Update"];[ask addButtonWithTitle:@"Not Now"];
 if([ask runModal]!=NSAlertFirstButtonReturn)return;
 NSURL *dmgURL=[self assetURL:BuilderAsset],*sumURL=[self assetURL:[BuilderAsset stringByAppendingString:@".sha256"]];
 NSURL *updates=[[NSFileManager.defaultManager URLsForDirectory:NSCachesDirectory inDomains:NSUserDomainMask].firstObject URLByAppendingPathComponent:@"GearheadGarage/Updates" isDirectory:YES];
 self.updateButton.enabled=NO;self.playButton.enabled=NO;self.status.stringValue=@"Downloading the update...";
 [[NSURLSession.sharedSession dataTaskWithRequest:[self request:sumURL] completionHandler:^(NSData *data,NSURLResponse *response,NSError *error){
  NSString *text=data&&((NSHTTPURLResponse *)response).statusCode==200?[[NSString alloc] initWithData:data encoding:NSUTF8StringEncoding]:nil;
  NSString *expected=[text componentsSeparatedByCharactersInSet:NSCharacterSet.whitespaceAndNewlineCharacterSet].firstObject.lowercaseString;
  dispatch_async(dispatch_get_main_queue(),^{
   if(expected.length!=64){[self updateFailed:[@"Could not download the update's checksum. " stringByAppendingString:error.localizedDescription?:@""]];return;}
   [self downloadBuilder:dmgURL expected:expected into:updates];
  });
 }] resume];
}
- (void)downloadBuilder:(NSURL *)url expected:(NSString *)expected into:(NSURL *)updates{
 __weak Launcher *weakSelf=self;
 self.download=[NSURLSession.sharedSession downloadTaskWithRequest:[self request:url] completionHandler:^(NSURL *file,NSURLResponse *response,NSError *error){
  NSError *problem=error;NSURL *builder=nil;
  if(!problem&&((NSHTTPURLResponse *)response).statusCode!=200)problem=failure([NSString stringWithFormat:@"The download failed (HTTP %ld).",(long)((NSHTTPURLResponse *)response).statusCode]);
  if(!problem)builder=[Launcher unpackBuilder:file expected:expected into:updates error:&problem];
  dispatch_async(dispatch_get_main_queue(),^{
   Launcher *owner=weakSelf;if(!owner)return;
   if(!builder){[owner updateFailed:[@"The update could not be installed: " stringByAppendingString:problem.localizedDescription]];return;}
   [owner startBuilder:builder];
  });
 }];
 self.progressTimer=[NSTimer scheduledTimerWithTimeInterval:0.25 repeats:YES block:^(NSTimer *timer){
  Launcher *owner=weakSelf;NSURLSessionDownloadTask *task=owner.download;if(!task){[timer invalidate];return;}
  if(task.countOfBytesExpectedToReceive>0)owner.status.stringValue=[NSString stringWithFormat:@"Downloading the update... %d%%",(int)(100*task.countOfBytesReceived/task.countOfBytesExpectedToReceive)];
 }];
 [self.download resume];
}
// Runs on the download's queue: checks the disk image, then copies the Builder out of it.
+ (NSURL *)unpackBuilder:(NSURL *)file expected:(NSString *)expected into:(NSURL *)updates error:(NSError **)error{
 NSFileManager *files=NSFileManager.defaultManager;
 [files removeItemAtURL:updates error:nil];
 if(![files createDirectoryAtURL:updates withIntermediateDirectories:YES attributes:nil error:error])return nil;
 NSURL *dmg=[updates URLByAppendingPathComponent:BuilderAsset],*mount=[updates URLByAppendingPathComponent:@"mount" isDirectory:YES];
 if(![files moveItemAtURL:file toURL:dmg error:error])return nil;
 if(![fileSHA256(dmg) isEqualToString:expected]){*error=failure(@"the downloaded file failed its checksum, so it was discarded.");[files removeItemAtURL:dmg error:nil];return nil;}
 [files createDirectoryAtURL:mount withIntermediateDirectories:YES attributes:nil error:nil];
 NSString *output=runTool(@"/usr/bin/hdiutil",@[@"attach",@"-nobrowse",@"-readonly",@"-noautoopen",@"-mountpoint",mount.path,dmg.path]);
 if(output){*error=failure([@"the disk image could not be opened: " stringByAppendingString:output]);return nil;}
 NSURL *builder=[updates URLByAppendingPathComponent:BuilderName];
 output=runTool(@"/usr/bin/ditto",@[[mount URLByAppendingPathComponent:BuilderName].path,builder.path]);
 runTool(@"/usr/bin/hdiutil",@[@"detach",mount.path,@"-force"]);
 [files removeItemAtURL:dmg error:nil];[files removeItemAtURL:mount error:nil];
 if(output){*error=failure([@"the Builder could not be copied: " stringByAppendingString:output]);return nil;}
 if(![[NSBundle bundleWithURL:builder].bundleIdentifier isEqualToString:BuilderID]){*error=failure(@"the disk image does not hold Gearhead Garage Builder.");return nil;}
 return builder;
}
- (void)startBuilder:(NSURL *)builder{
 [self.progressTimer invalidate];self.progressTimer=nil;
 self.status.stringValue=@"Starting Gearhead Garage Builder...";
 NSURL *app=NSBundle.mainBundle.bundleURL;
 NSWorkspaceOpenConfiguration *configuration=[NSWorkspaceOpenConfiguration configuration];
 configuration.arguments=@[@"-gameCopy",app.path,@"-destination",app.URLByDeletingLastPathComponent.path,@"-AutoBuild",@"YES",@"-OpenWhenDone",@"YES"];
 [NSWorkspace.sharedWorkspace openApplicationAtURL:builder configuration:configuration completionHandler:^(NSRunningApplication *running,NSError *error){
  dispatch_async(dispatch_get_main_queue(),^{
   if(!running){[self updateFailed:[@"Gearhead Garage Builder could not be started: " stringByAppendingString:error.localizedDescription?:@""]];return;}
   self.download=nil;[NSApp terminate:nil];
  });
 }];
}
- (void)play:(id)sender{
 (void)sender;if(self.game||self.download)return;
 NSUserDefaults *settings=NSUserDefaults.standardUserDefaults;
 BOOL software=self.graphics.indexOfSelectedItem==1;
 [settings setObject:software?@"software":@"d3d" forKey:@"Graphics"];
 [settings setBool:self.fullscreen.state==NSControlStateValueOn forKey:@"Fullscreen"];
 [settings setBool:self.sound.state==NSControlStateValueOn forKey:@"Sound"];
 NSFileManager *files=NSFileManager.defaultManager;NSError *error=nil;
 [files createDirectoryAtURL:self.saves withIntermediateDirectories:YES attributes:nil error:nil];
 NSDateFormatter *format=[NSDateFormatter new];format.locale=[[NSLocale alloc] initWithLocaleIdentifier:@"en_US_POSIX"];format.dateFormat=@"yyyyMMdd-HHmmss";
 NSURL *log=[self.logs URLByAppendingPathComponent:[NSString stringWithFormat:@"Gearhead-%@.log",[format stringFromDate:NSDate.date]]];
 if(![files createFileAtPath:log.path contents:nil attributes:nil]){alert(@"Cannot create the game log.");return;}
 NSFileHandle *output=[NSFileHandle fileHandleForWritingAtPath:log.path];
 NSTask *task=[NSTask new];task.executableURL=[NSBundle.mainBundle.executableURL.URLByDeletingLastPathComponent URLByAppendingPathComponent:@"GearheadEngine"];
 task.arguments=@[[self url:@"linked.exe"].path,[self url:@"Game"].path];task.currentDirectoryURL=self.support;
 NSMutableDictionary *environment=[NSProcessInfo.processInfo.environment mutableCopy];
 BOOL diagnostics=[NSProcessInfo.processInfo.arguments containsObject:@"--diagnostics"];
 if(!diagnostics)for(NSString *key in environment.allKeys.copy)if([key hasPrefix:@"GEARHEAD_"])[environment removeObjectForKey:key];
 NSMutableArray *args=[NSMutableArray new];
 if(software)[args addObjectsFromArray:@[@"-Vid",@"soft"]];
 if(self.sound.state!=NSControlStateValueOn)[args addObject:@"-NoSound"];
 environment[@"GEARHEAD_ARGS"]=[args componentsJoinedByString:@" "];
 environment[@"GEARHEAD_SAVE_DIR"]=self.saves.path;environment[@"GEARHEAD_REGFILE"]=[self.saves URLByAppendingPathComponent:@"registry"].path;
 environment[@"GEARHEAD_FULLSCREEN"]=self.fullscreen.state==NSControlStateValueOn?@"1":@"0";
 environment[@"GEARHEAD_AUDIO"]=self.sound.state==NSControlStateValueOn?@"1":@"0";
 if(!diagnostics||!environment[@"GEARHEAD_TIMEOUT"])environment[@"GEARHEAD_TIMEOUT"]=@"0";
 task.environment=environment;task.standardOutput=output;task.standardError=output;
 __weak Launcher *weakSelf=self;
 task.terminationHandler=^(NSTask *ended){dispatch_async(dispatch_get_main_queue(), ^{
  [output closeFile];Launcher *owner=weakSelf;owner.game=nil;owner.playButton.enabled=YES;
  owner.status.stringValue=owner.latestRelease?[NSString stringWithFormat:@"Version %@ is available",stripV(owner.latestRelease[@"tag_name"])]:@"Ready";
  [NSApp setActivationPolicy:NSApplicationActivationPolicyRegular];[owner.window makeKeyAndOrderFront:nil];[NSApp activateIgnoringOtherApps:YES];
  if(ended.terminationStatus)alert(@"The game stopped unexpectedly. Open Logs to see the diagnostic log.");
 });};
 if(![task launchAndReturnError:&error]){[output closeFile];alert(error.localizedDescription);return;}
 self.game=task;self.playButton.enabled=NO;self.status.stringValue=@"Game running";[self.window orderOut:nil];[NSApp setActivationPolicy:NSApplicationActivationPolicyAccessory];
}
- (void)openSaves:(id)sender{(void)sender;[NSWorkspace.sharedWorkspace openURL:self.saves];}
- (void)openLogs:(id)sender{(void)sender;[NSWorkspace.sharedWorkspace openURL:self.logs];}
- (void)openHelp:(id)sender{(void)sender;[NSWorkspace.sharedWorkspace openURL:[self url:@"Game/Help/index.htm"]];}
- (BOOL)applicationShouldTerminateAfterLastWindowClosed:(NSApplication *)application{(void)application;return self.game==nil;}
- (NSApplicationTerminateReply)applicationShouldTerminate:(NSApplication *)sender{(void)sender;return self.game||(self.download&&self.progressTimer)?NSTerminateCancel:NSTerminateNow;}
@end
int main(void){@autoreleasepool{
 if([NSProcessInfo.processInfo.arguments containsObject:@"--check"]){
  Launcher *check=[Launcher new];check.resources=NSBundle.mainBundle.resourceURL;
  if(![check validGame]){fputs("Prepared game data does not match this build\n",stderr);return 1;}
  puts("Prepared game checksum verified");return 0;
 }
 [NSApplication sharedApplication];[NSApp setActivationPolicy:NSApplicationActivationPolicyRegular];
 static Launcher *delegate;delegate=[Launcher new];NSApp.delegate=delegate;[NSApp run];
}return 0;}
