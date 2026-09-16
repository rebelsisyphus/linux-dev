from pathlib import Path
import json
import shutil
import subprocess
import sys
import time

root = Path(__file__).resolve().parent
state = json.loads((root / 'state.json').read_text())
source = Path(state['source'])
artifact = Path(state['artifact_dir'])
label = sys.argv[1]
assert label in ('prove-rcu-n', 'prove-rcu-y')
seed = artifact / ('verification/' + label + '-seed.config')
build = root / ('build-' + label)
build.mkdir()
shutil.copyfile(seed, build / '.config')
logs = artifact / 'verification/logs'
commands = []
targets = [
    'net/netfilter/nf_tables_api.o',
    'net/netfilter/nf_tables_core.o',
    'net/netfilter/nft_lookup.o',
    'net/netfilter/nft_set_pipapo.o',
    'net/netfilter/nft_set_pipapo_avx2.o',
    'net/netfilter/nft_set_hash.o',
    'net/netfilter/nft_set_rbtree.o',
    'net/netfilter/nft_set_bitmap.o',
    'net/netfilter/nft_dynset.o',
    'net/netfilter/nfnetlink.o',
]
started = time.monotonic()

def run_make(args, suffix):
    command = ['make', '-C', str(source), 'O=' + str(build), *args]
    commands.append(command)
    with (logs / (label + '-' + suffix + '.log')).open('w') as stream:
        result = subprocess.run(command, stdout=stream, stderr=subprocess.STDOUT)
    print(label, suffix, 'exit', result.returncode, flush=True)
    return result.returncode

assert subprocess.check_output(['git', 'rev-parse', 'HEAD'], cwd=source,
                               text=True).strip() == state['integration_head']
status = run_make(['olddefconfig'], 'config')
if status == 0:
    config = (build / '.config').read_text()
    assert ('CONFIG_PROVE_RCU=y\n' in config) == (label == 'prove-rcu-y')
    assert ('CONFIG_PROVE_LOCKING=y\n' in config) == (label == 'prove-rcu-y')
    assert ('CONFIG_NF_TABLES=m\n' in config) == (label == 'prove-rcu-n')
    shutil.copyfile(build / '.config', artifact / ('verification/' + label + '.config'))
    status = run_make(['-j4', 'KCFLAGS=-Werror=unused-function', *targets], 'objects')

result = {
    'profile': label,
    'source_commit': state['integration_head'],
    'source_tree': state['integration_tree'],
    'build_dir': str(build),
    'seed_config': str(seed),
    'commands': commands,
    'targets': targets,
    'exit_code': status,
    'elapsed_seconds': round(time.monotonic() - started, 2),
    'runtime_test': False,
}
(logs / (label + '-result.json')).write_text(json.dumps(result, indent=2) + '\n')
sys.exit(status)
