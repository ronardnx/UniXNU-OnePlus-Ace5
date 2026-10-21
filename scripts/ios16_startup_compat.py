"""Explicit no-baseband/audio/GPU startup configuration for the Ace 5 port.

These changes preserve first-boot migration, Setup and account/activation state.
They do not supply cellular, audio, recording or DRM functionality.
"""
import hashlib
import plistlib
from ios16_root_layout import MIGRATION, VERSION

LAUNCHD = '/System/Library/xpc/launchd.plist'
MEDIA = '/System/Library/Frameworks/MediaToolbox.framework/{}/SubServerConfiguration.plist'
VIRTUAL_AUDIO = '/Library/Audio/Plug-Ins/HAL/VirtualAudio.plugin'
DISABLED_AUDIO = '/Library/Audio/Plug-Ins/HAL.disabled'
SPRINGBOARD = '/private/var/mobile/Library/Preferences/com.apple.springboard.plist'
JOBS = tuple('/System/Library/LaunchDaemons/'+name+'.plist' for name in (
    'com.apple.CommCenter', 'com.apple.CommCenter-noncellular',
    'com.apple.CommCenterRootHelper', 'com.apple.CommCenterMobileHelper'))


def prepare_startup(root):
    if plistlib.loads(root.read(VERSION))['ProductBuildVersion'] != '20H392' or root.exists(MIGRATION):
        raise ValueError('Startup compatibility requires fresh 20H392 userdata')
    if not root.exists(VIRTUAL_AUDIO) or root.exists(DISABLED_AUDIO+'/VirtualAudio.plugin'):
        raise ValueError('Unexpected audio plugin layout')
    # Validate all source configurations before making changes.
    original = {name:root.read(name) for name in (LAUNCHD, *(MEDIA.format(x) for x in ('D22','D221','Default')))}
    updated = {name:plistlib.loads(data) for name, data in original.items()}
    jobs = updated[LAUNCHD]['LaunchDaemons']
    for name in JOBS:
        if name not in jobs or not jobs[name]['Label'].startswith('com.apple.CommCenter'):
            raise ValueError('CommCenter job inventory changed')
    for name in JOBS: del jobs[name]
    for name, value in updated.items():
        if name != LAUNCHD: value['StartRecorderServer'] = False
    if root.exists(SPRINGBOARD): raise ValueError('Expected unconfigured SpringBoard preferences')
    changes = []
    for name, value in updated.items():
        data = plistlib.dumps(value, fmt=plistlib.FMT_BINARY, sort_keys=False)
        root.write(name, data)
        if root.read(name) != data: raise ValueError('Configuration write verification failed')
        changes.append({'path':name, 'before_sha256':hashlib.sha256(original[name]).hexdigest(),
                        'after_sha256':hashlib.sha256(data).hexdigest()})
    root.directory(DISABLED_AUDIO, 0)
    root.move(VIRTUAL_AUDIO, DISABLED_AUDIO+'/VirtualAudio.plugin')
    for name in ('/private/var/mobile/Library', '/private/var/mobile/Library/Preferences'):
        root.directory(name, 501)
    preferences = {'BKNoWatchdogs':True, 'FBLaunchWatchdogExceptions':{'com.apple.purplebuddy':100}}
    data = plistlib.dumps(preferences, fmt=plistlib.FMT_BINARY)
    root.write(SPRINGBOARD, data, uid=501, mode=0o600)
    if plistlib.loads(root.read(SPRINGBOARD)) != preferences or root.exists(MIGRATION):
        raise ValueError('Startup preferences verification failed')
    return {'startup_compatibility':{
        'disabled_launchd_jobs':list(JOBS), 'configuration_changes':changes,
        'audio_plugin_moved_to':DISABLED_AUDIO+'/VirtualAudio.plugin',
        'springboard_preferences':preferences,
        'reason':'No baseband/audio/GPU drivers; single-CPU software GUI bring-up',
        'setup_completion_seeded':False, 'activation_state_seeded':False,
        'migration_completion_seeded':False, 'hardware_verified':False}}
