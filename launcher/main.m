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

@interface Launcher : NSObject <NSApplicationDelegate,NSWindowDelegate>
@property NSWindow *window;
@property NSPopUpButton *graphics;
@property NSButton *fullscreen,*sound,*playButton;
@property NSTextField *status;
@property NSURL *resources,*support,*saves,*logs;
@property NSTask *game;
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
 [settings registerDefaults:@{@"Graphics":@"d3d",@"Fullscreen":@NO,@"Sound":@YES}];
 self.window=[[NSWindow alloc] initWithContentRect:NSMakeRect(0,0,460,330) styleMask:NSWindowStyleMaskTitled|NSWindowStyleMaskClosable|NSWindowStyleMaskMiniaturizable backing:NSBackingStoreBuffered defer:NO];
 self.window.title=@"Gearhead Garage";self.window.delegate=self;self.window.releasedWhenClosed=NO;
 NSStackView *stack=[NSStackView new];stack.orientation=NSUserInterfaceLayoutOrientationVertical;stack.alignment=NSLayoutAttributeLeading;stack.spacing=12;stack.translatesAutoresizingMaskIntoConstraints=NO;
 [self.window.contentView addSubview:stack];
 [NSLayoutConstraint activateConstraints:@[[stack.leadingAnchor constraintEqualToAnchor:self.window.contentView.leadingAnchor constant:22],[stack.trailingAnchor constraintEqualToAnchor:self.window.contentView.trailingAnchor constant:-22],[stack.topAnchor constraintEqualToAnchor:self.window.contentView.topAnchor constant:22],[stack.bottomAnchor constraintLessThanOrEqualToAnchor:self.window.contentView.bottomAnchor constant:-18]]];
 NSTextField *title=label(@"Gearhead Garage");title.font=[NSFont systemFontOfSize:24 weight:NSFontWeightSemibold];[stack addArrangedSubview:title];
 [stack addArrangedSubview:note(@"Snap-on Gearhead Garage, Build 131")];
 self.graphics=[[NSPopUpButton alloc] initWithFrame:NSZeroRect pullsDown:NO];
 [self.graphics addItemsWithTitles:@[@"Direct3D (full color)",@"Software (256 colors)"]];
 [self.graphics selectItemAtIndex:[[settings stringForKey:@"Graphics"] isEqualToString:@"software"]?1:0];
 NSGridView *grid=[NSGridView gridViewWithViews:@[@[label(@"Graphics"),self.graphics]]];grid.columnSpacing=18;[stack addArrangedSubview:grid];
 self.fullscreen=check(@"Fullscreen (Control-Command-F or F11 switches while playing)",[settings boolForKey:@"Fullscreen"]);
 self.sound=check(@"Sound",[settings boolForKey:@"Sound"]);
 [stack addArrangedSubview:self.fullscreen];[stack addArrangedSubview:self.sound];
 [stack addArrangedSubview:note(@"The game runs at its original 640x480, scaled to the window with the\noriginal 4:3 picture. Your mechanics and snapshots stay in Saves.")];
 self.status=label(@"Ready");self.status.font=[NSFont systemFontOfSize:11];[stack addArrangedSubview:self.status];
 self.playButton=[NSButton buttonWithTitle:@"Play" target:self action:@selector(play:)];self.playButton.keyEquivalent=@"\r";
 NSButton *saves=[NSButton buttonWithTitle:@"Open Saves" target:self action:@selector(openSaves:)];
 NSButton *logs=[NSButton buttonWithTitle:@"Open Logs" target:self action:@selector(openLogs:)];
 NSButton *help=[NSButton buttonWithTitle:@"Game Help" target:self action:@selector(openHelp:)];
 NSStackView *buttons=[NSStackView stackViewWithViews:@[self.playButton,saves,logs,help]];buttons.spacing=10;[stack addArrangedSubview:buttons];
 NSMenu *menu=[NSMenu new];NSMenuItem *app=[NSMenuItem new];[menu addItem:app];NSMenu *appMenu=[NSMenu new];app.submenu=appMenu;
 [appMenu addItemWithTitle:@"Quit Gearhead Garage" action:@selector(terminate:) keyEquivalent:@"q"];NSApp.mainMenu=menu;
 [self.window center];[self.window makeKeyAndOrderFront:nil];[NSApp activateIgnoringOtherApps:YES];
 if([NSProcessInfo.processInfo.arguments containsObject:@"--play"])[self play:nil];
}
- (void)play:(id)sender{
 (void)sender;if(self.game)return;
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
  [output closeFile];Launcher *owner=weakSelf;owner.game=nil;owner.playButton.enabled=YES;owner.status.stringValue=@"Ready";
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
- (NSApplicationTerminateReply)applicationShouldTerminate:(NSApplication *)sender{(void)sender;return self.game?NSTerminateCancel:NSTerminateNow;}
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
