"""Run one owned experiment; keep logs out of model context. Standard library only."""
import argparse
import hashlib
import json
import os
from pathlib import Path
import subprocess
import sys
import time


def fingerprint(paths):
    result = {}
    for value in paths:
        path = Path(value).resolve(strict=True)
        digest = hashlib.sha256()
        with path.open('rb') as stream:
            for block in iter(lambda: stream.read(1024 * 1024), b''):
                digest.update(block)
        result[str(path)] = digest.hexdigest()
    return result


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--cwd', required=True)
    parser.add_argument('--state', required=True)
    parser.add_argument('--timeout', type=float, required=True)
    parser.add_argument('--heartbeat', type=float, default=45)
    parser.add_argument('--fingerprint', action='append', default=[])
    parser.add_argument('--unset-env', action='append', default=[])
    parser.add_argument('--set-env', action='append', default=[])
    parser.add_argument('command', nargs=argparse.REMAINDER)
    args = parser.parse_args()
    command = args.command[1:] if args.command[:1] == ['--'] else args.command
    if not command or args.timeout <= 0 or not 0 < args.heartbeat <= 50:
        parser.error('command, positive timeout, and heartbeat in (0, 50] required')
    cwd = Path(args.cwd).resolve(strict=True)
    state = Path(args.state).resolve()
    if state.exists():
        parser.error('state path already exists; choose a new run name')
    env = os.environ.copy()
    for key in args.unset_env:
        env.pop(key, None)
    for assignment in args.set_env:
        key, separator, value = assignment.partition('=')
        if not separator or not key:
            parser.error('--set-env requires KEY=VALUE')
        env[key] = value
    before = fingerprint(args.fingerprint)
    state.parent.mkdir(parents=True, exist_ok=True)
    stdout_path = state.with_suffix('.stdout.log')
    stderr_path = state.with_suffix('.stderr.log')
    if stdout_path.exists() or stderr_path.exists():
        parser.error('log paths already exist; choose a new run name')
    started = time.monotonic()
    record = {'status': 'starting', 'stdout': str(stdout_path), 'stderr': str(stderr_path),
              'fingerprints_before': before}

    def save():
        temporary = state.with_suffix(state.suffix + '.tmp')
        temporary.write_text(json.dumps(record, indent=2), encoding='utf-8')
        temporary.replace(state)

    def stop_owned_tree(process):
        if os.name == 'nt':
            subprocess.run(['taskkill.exe', '/PID', str(process.pid), '/T', '/F'],
                           stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL,
                           creationflags=subprocess.CREATE_NO_WINDOW, check=False)
        else:
            import signal
            os.killpg(process.pid, signal.SIGKILL)
        process.wait(timeout=10)

    save()
    try:
        with stdout_path.open('xb') as output, stderr_path.open('xb') as errors:
            options = {'creationflags': subprocess.CREATE_NO_WINDOW} if os.name == 'nt' else {'start_new_session': True}
            process = subprocess.Popen(command, cwd=cwd, env=env, stdout=output, stderr=errors, **options)
            record.update(status='running', pid=process.pid)
            save()
            print(json.dumps({'status': 'running', 'state': str(state), 'pid': process.pid}), flush=True)
            next_heartbeat = args.heartbeat
            try:
                while process.poll() is None:
                    elapsed = time.monotonic() - started
                    if elapsed >= args.timeout:
                        stop_owned_tree(process)
                        record['status'] = 'timeout'
                        break
                    if elapsed >= next_heartbeat:
                        record['elapsed_s'] = round(elapsed, 1)
                        save()
                        print(json.dumps({'status': 'running', 'elapsed_s': record['elapsed_s']}), flush=True)
                        next_heartbeat = elapsed + args.heartbeat
                    time.sleep(min(0.2, max(0.01, args.timeout - elapsed)))
            except BaseException:
                if process.poll() is None:
                    stop_owned_tree(process)
                raise
            record['child_exit_code'] = process.returncode
            if record['status'] != 'timeout':
                record['status'] = 'passed' if process.returncode == 0 else 'failed'
    except BaseException as error:
        record.update(status='failed', error=f'{type(error).__name__}: {error}')
    try:
        after = fingerprint(args.fingerprint)
        record['fingerprints_after'] = after
        if before != after:
            record['changed_inputs'] = [p for p in before if before[p] != after.get(p)]
            if record['status'] == 'passed':
                record['status'] = 'censored'
    except OSError as error:
        record['fingerprint_error'] = str(error)
        if record['status'] == 'passed':
            record['status'] = 'censored'
    record['elapsed_s'] = round(time.monotonic() - started, 2)
    save()
    print(json.dumps({'status': record['status'], 'elapsed_s': record['elapsed_s'], 'state': str(state)}), flush=True)
    return 0 if record['status'] == 'passed' else 1


if __name__ == '__main__':
    sys.exit(main())
