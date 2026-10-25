"""Small public manifest: no source-machine paths, commands, credentials or audio."""
import hashlib,re,subprocess
from pathlib import Path
FLAGS=('normal_app_integration_qualified','third_party_qualified','runtime_qualified')
def valid_relative(path):
    p=Path(path)
    return bool(path) and not p.is_absolute() and '..' not in p.parts and '\\' not in path

def sanitized_manifest(raw, helper, root):
    actual=hashlib.sha256(helper.read_bytes()).hexdigest()
    if raw.get('schema')!='rysup.fl-ara-helper/1' or raw.get('binary')!=helper.name or raw.get('runtime_ready') is not True:
        raise ValueError('Helper admission is absent or unready')
    result={'schema':raw['schema'],'binary':helper.name,'sha256':actual,'runtime_ready':True,
            'runtime_admission_scope':raw.get('runtime_admission_scope','owned-qualification-candidate')}
    if result['runtime_admission_scope'] not in {'bare-startup-test-only','owned-qualification-candidate','qualified-runtime'}:raise ValueError('Unrecognized runtime admission scope')
    for name in FLAGS:result[name]=raw.get(name) is True
    if 'routing_only' in raw and not isinstance(raw['routing_only'],bool):raise ValueError('Expected boolean routing-only scope')
    if raw.get('routing_only') is True:
        restrictions={'routing_only':True,'device_start_fix':False,'no_device_start_fix':True,
                      'audio_io_qualification':False,'no_audio_io_qualification':True,
                      'retains_immutable_r5_device_start_guards':True}
        scope='graph-model-native-save-offline-export-testing-only'
        if any(raw.get(k) is not v for k,v in restrictions.items()) or raw.get('intended_runtime_scope')!=scope:
            raise ValueError('Routing-only restrictions must be explicit and consistent')
        if result['runtime_admission_scope']!='owned-qualification-candidate' or any(result[k] for k in FLAGS):
            raise ValueError('Routing-only candidate cannot claim runtime qualification')
        result.update(restrictions);result['intended_runtime_scope']=scope
    if 'candidate_kind' in raw:
        kinds={'full-compiled-device-gate-unqualified','full-compiled-cache-queue-and-process-admission-unqualified','diagnostic-only-setup-and-physical-identity'}
        if raw['candidate_kind'] not in kinds or raw.get('device_start_admission_compiled') is not True or raw.get('actual_audio_io_qualified') is not False:
            raise ValueError('Compiled device-gate candidate metadata is inconsistent')
        if raw.get('routing_only') is True or result['runtime_admission_scope']!='owned-qualification-candidate' or any(result[k] for k in FLAGS):
            raise ValueError('Compiled device-gate candidate remains runtime unqualified')
        result.update(candidate_kind=raw['candidate_kind'],device_start_admission_compiled=True,actual_audio_io_qualified=False)
        if raw['candidate_kind']=='full-compiled-cache-queue-and-process-admission-unqualified':
            for field in ('strong_process_admission_compiled','authority_only_internal_cache_invalidation_compiled'):
                if raw.get(field) is not True:raise ValueError('Cache/process-admission compile metadata is inconsistent')
                result[field]=True
        if raw['candidate_kind']=='diagnostic-only-setup-and-physical-identity':
            if raw.get('diagnostic_only') is not True or raw.get('guard_logic_changed') is not False or raw.get('strong_process_admission_compiled') is not True:
                raise ValueError('Diagnostic-only candidate metadata is inconsistent')
            result.update(diagnostic_only=True,guard_logic_changed=False,strong_process_admission_compiled=True)
    revision=raw.get('source_revision')
    if revision is None:
        revision=subprocess.check_output(['git','-C',str(root),'rev-parse','HEAD'],text=True).strip()
    if not isinstance(revision,str) or not re.fullmatch('[0-9a-f]{40}',revision):raise ValueError('Expected a complete source commit')
    result['source_revision']=revision
    hashes={}
    if 'input_relative_hashes' in raw:
        for name,seal in raw['input_relative_hashes'].items():
            if not valid_relative(name) or not isinstance(seal,str) or not re.fullmatch('[0-9a-f]{64}',seal):raise ValueError('Invalid relative input seal')
            hashes[name]=seal
    else:
        bridge=root/'native/bridge'
        if bridge.is_dir():
            for folder in ('host','native','legacy'):
                for file in sorted((bridge/folder).iterdir()):
                    if file.is_file() and not file.is_symlink() and file.suffix in {'.h','.hpp','.cpp','.mm','.py'}:
                        hashes[str(file.relative_to(root))]=hashlib.sha256(file.read_bytes()).hexdigest()
    result['input_relative_hashes']=hashes
    # Public build profile is explicit and contains no arbitrary compile-manifest strings.
    result['profile']={'architecture':'arm64','minimum_macos':'15.4','fl_version':'26.1.7.5421','initial_sample_rate':48000,
                       'test_audio_device_uid':'Pro Tools Audio Bridge 2-A_UID','python_runtime_required':False}
    return result
