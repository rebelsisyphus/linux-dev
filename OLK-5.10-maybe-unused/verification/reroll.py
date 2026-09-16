from pathlib import Path
import json
import os
import re
import subprocess

root = Path(__file__).resolve().parent
state = json.loads((root / 'state.json').read_text())
repo = Path(state['repo'])
source = Path(state['source'])
artifact = Path(state['artifact_dir'])
previous = repo / 'cve_patches/CVE-2026-72252/OLK-5.10-review-fixes'
manifest = json.loads((previous / 'verification/manifest.json').read_text())[7:]
assert len(manifest) == 18
declaration = 'static bool nft_pipapo_transaction_mutex_held(const struct nft_set *set)'
annotated = declaration.replace('static bool ', 'static bool __maybe_unused ')
log = []

def git(*args, cwd=source, data=None, env=None):
    result = subprocess.run(['git', *args], cwd=cwd, input=data,
                            text=True, stdout=subprocess.PIPE,
                            stderr=subprocess.PIPE, env=env)
    if result.returncode:
        raise RuntimeError(f'{args}: {result.stderr}')
    return result.stdout

assert git('rev-parse', 'HEAD').strip() == state['base']
assert not git('status', '--porcelain')

def replay(original, patch, note=None):
    message = git('show', '-s', '--format=format:%B', original, cwd=repo)
    if note:
        before, after = message.split('\nConflicts:\n', 1)
        message = before + '\n' + note + '\n\nConflicts:\n' + after
        signoff = f"Signed-off-by: {state['author_name']} <{state['author_email']}>"
        if signoff not in message:
            message = message.rstrip() + '\n' + signoff + '\n'
    identity = git('show', '-s', '--format=%an%x00%ae%x00%aI', original,
                   cwd=repo).strip().split('\0')
    env = os.environ.copy()
    env.update(dict(zip(('GIT_AUTHOR_NAME', 'GIT_AUTHOR_EMAIL', 'GIT_AUTHOR_DATE'),
                        identity)))
    git('apply', '--index', '-', data=patch)
    git('diff', '--cached', '--check')
    path = root / 'commit-message.txt'
    path.write_text(message)
    output = git('commit', '--cleanup=verbatim', '--file', str(path), env=env)
    local = git('rev-parse', 'HEAD').strip()
    log.append(output)
    return local

for index, item in enumerate(manifest, 1):
    original = item['local']
    patch = git('diff', '--binary', original + '^', original, cwd=repo)
    note = None
    if item['upstream'].startswith('b9f052dc68f6'):
        assert patch.count(declaration) == 1
        patch = patch.replace(declaration, annotated)
        note = (
            '[Dong Chenchen: mark nft_pipapo_transaction_mutex_held() __maybe_unused.\n'
            'The target lacks 65e9eb1ccfe5, so !CONFIG_PROVE_RCU drops the condition\n'
            'and all calls to this helper. Keep the build fix local to PIPAPO.]'
        )
    elif item['upstream'].startswith('3f1d886cc7c3'):
        assert patch.count(declaration) == 2
        patch = patch.replace(declaration, annotated)
        note = (
            '[Dong Chenchen: retain __maybe_unused when moving the lockdep helper\n'
            'for the target\'s !CONFIG_PROVE_RCU build.]'
        )
    item['previous_25_local'] = original
    item['local'] = replay(original, patch, note)
    item['number'] = index
    item['local_annotation_change'] = note is not None
    pipapo = (source / 'net/netfilter/nft_set_pipapo.c').read_text()
    if index >= 2:
        assert pipapo.count(annotated) == 1
        assert declaration not in pipapo
    print(index, item['local'][:12], item['subject'], flush=True)

state['series_head'] = git('rev-parse', 'HEAD').strip()
git('branch', state['series_branch'], state['series_head'])
integrations = []
sources = git('rev-list', '--reverse', state['previous_series_head'] + '..' +
              state['previous_integration_head'], cwd=repo).splitlines()
assert len(sources) == 7
for original in sources:
    patch = git('diff', '--binary', original + '^', original, cwd=repo)
    local = replay(original, patch)
    integrations.append({'previous': original, 'local': local,
                         'subject': git('show', '-s', '--format=%s', local).strip()})
    print('80668', local[:12], integrations[-1]['subject'], flush=True)

state['integration_head'] = git('rev-parse', 'HEAD').strip()
state['integration_tree'] = git('rev-parse', 'HEAD^{tree}').strip()
assert not git('status', '--porcelain')
for path in (root / 'state.json', artifact / 'verification/state.json'):
    path.write_text(json.dumps(state, indent=2) + '\n')
(artifact / 'verification/manifest.json').write_text(json.dumps(manifest, indent=2) + '\n')
(artifact / 'verification/integration-replay.json').write_text(json.dumps(integrations, indent=2) + '\n')
(artifact / 'verification/logs/reroll.log').write_text(''.join(log))
print(json.dumps(state, indent=2))
