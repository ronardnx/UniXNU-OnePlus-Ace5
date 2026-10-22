import importlib.util
import json
from pathlib import Path
import plistlib
import struct
import sys
import tempfile
import unittest

ROOT=Path(__file__).resolve().parents[1]
sys.path.insert(0,str(ROOT/'scripts'))
import ios16_terminal
spec=importlib.util.spec_from_file_location('stage_debug',ROOT/'scripts/stage-debug-userland.py')
stage=importlib.util.module_from_spec(spec);spec.loader.exec_module(stage)


def signed_fixture(platform=2,filetype=2):
    # Parser fixture, not an executable or a production signature.
    cd=bytearray(44)
    struct.pack_into('>II',cd,0,0xfade0c02,len(cd));cd[37]=2
    signature=struct.pack('>IIIII',0xfade0cc0,64,1,0,20)+cd
    return (struct.pack('<8I',0xfeedfacf,0x100000c,0,filetype,2,40,0,0)+
            struct.pack('<4I',0x1d,16,72,len(signature))+
            struct.pack('<6I',0x32,24,platform,0x100000,0x100000,0)+signature)


class MemoryRoot:
    def __init__(self):
        self.files={stage.CACHE:plistlib.dumps({'LaunchDaemons':{'existing':{'Label':'existing'}}})}
        self.directories=set()
    def read(self,name):return self.files[name]
    def exists(self,name):return name in self.files or name in self.directories
    def write(self,name,data,**kwargs):self.files[name]=data
    def directory(self,name,uid):self.directories.add(name)


class DebugUserland(unittest.TestCase):
    def test_stage_updates_cached_launchd_job_and_trust(self):
        root=MemoryRoot();binary=signed_fixture()
        manifest=stage.stage(root,binary,{'bin/sh':binary,'bin/ls':binary,'lib/libtest.dylib':signed_fixture(filetype=6)})
        jobs=plistlib.loads(root.read(stage.CACHE))['LaunchDaemons']
        self.assertIn('existing',jobs)
        self.assertEqual(jobs[stage.JOB]['ProgramArguments'],['/usr/local/giulia/giulia-debugd','/usr/local/giulia/bin/sh'])
        with tempfile.TemporaryDirectory() as directory:
            path=Path(directory)/'manifest.json';path.write_text(json.dumps(manifest))
            code=ios16_terminal.debug_trust_source(path)
            self.assertIn('enabled!=1',code)
            self.assertIn('length!=20',code)
            self.assertIn('memcmp(hash,hashes[i],20)',code)
            manifest['files']['/usr/bin/other']=manifest['files']['/usr/local/giulia/bin/sh']
            path.write_text(json.dumps(manifest))
            with self.assertRaises(ValueError):ios16_terminal.debug_trust_source(path)

    def test_invalid_bundle_fails_before_writing(self):
        for tools in ({}, {'bin/sh':b'unsigned'}, {'bin/../sh':signed_fixture()},
                      {'bin/sh':signed_fixture(platform=1)}, {'bin/sh':signed_fixture(filetype=6)}):
            root=MemoryRoot();before=dict(root.files)
            with self.assertRaises(ValueError):stage.stage(root,signed_fixture(),tools)
            self.assertEqual(root.files,before)
            self.assertFalse(root.directories)

    def test_no_staged_root_overwrite(self):
        root=MemoryRoot();binary=signed_fixture()
        stage.stage(root,binary,{'bin/sh':binary})
        before=dict(root.files)
        with self.assertRaises(ValueError):stage.stage(root,binary,{'bin/sh':binary})
        self.assertEqual(root.files,before)

    def test_profile_usb_transport_hooks_and_source_drift(self):
        reference=ROOT.parent/'reference-vermeer/xnu/vermeer'
        if not reference.is_dir():self.skipTest('Pinned local reference not present')
        def edit(path,old,new,count=1):
            data=path.read_text()
            if data.count(old)!=count:raise ValueError('source drift')
            path.write_text(data.replace(old,new))
        with tempfile.TemporaryDirectory() as directory:
            usb=Path(directory)/'usb.c';block=Path(directory)/'block.c'
            usb.write_text((reference/'vermeer_usb_debug.c').read_text())
            block.write_text((reference/'vermeer_ufs_block.c').read_text())
            ios16_terminal.terminal_sources(usb,block,edit)
            result=usb.read_text()
            self.assertIn('gs_transmit((char *)(u.dma+TX),u.mps-1',result)
            self.assertIn('gs_receive((const uint8_t *)(u.dma+RX)',result)
            self.assertIn('gs_register();\n    while (!u.failed)',result)
            with self.assertRaises(ValueError):ios16_terminal.terminal_sources(usb,block,edit)


if __name__=='__main__':unittest.main()
