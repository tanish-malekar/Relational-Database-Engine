"""Count actual injected process exits and verify acknowledged transactions and atomicity."""
import argparse
import json
import os
from pathlib import Path
import subprocess
import tempfile


def main():
    p = argparse.ArgumentParser()
    p.add_argument('--worker', type=Path, default=Path('build/sql_crash_worker'))
    p.add_argument('--crashes', type=int, default=500)
    p.add_argument('--output', type=Path, required=True)
    args = p.parse_args()
    if args.crashes < 1:
        raise ValueError('Crash budget must be positive')
    worker = str(args.worker.resolve())
    env = {k: v for k, v in os.environ.items() if k not in {'SQL_CRASH_POINT', 'SQL_CRASH_OCCURRENCE'}}
    def invoke(root, mode, point=None):
        child = {**env, **({'SQL_CRASH_POINT': point} if point else {})}
        return subprocess.run([worker, str(root), mode], env=child, text=True,
                              stdout=subprocess.PIPE, stderr=subprocess.PIPE, timeout=20)
    points = ['update_appended', 'updates_forced', 'steal_page_written', 'commit_forced', 'data_forced', 'transaction_ended']
    recovery_points = ['recovery_redo', 'recovery_clr_forced', 'recovery_undo']
    injected = failures = lost = case = 0
    with args.output.open('x') as ledger:
        while injected < args.crashes:
            case += 1
            nested = case % 4 == 0 and injected + 1 < args.crashes
            point = 'updates_forced' if nested else points[(case - 1) % len(points)]
            record = {'case': case, 'forward_point': point, 'recovery_point': None}
            with tempfile.TemporaryDirectory(prefix='sqldb-crash-') as directory:
                root = Path(directory)
                seeded = invoke(root, 'seed')
                if seeded.returncode:
                    raise RuntimeError(seeded.stderr)
                crashed = invoke(root, 'work', point)
                if crashed.returncode != 91:
                    raise RuntimeError(f'Injection point was not reached: {point}: {crashed.stderr}')
                injected += 1
                if (root / 'ack.txt').read_text() != '100\n':
                    raise RuntimeError('Worker did not durably record its acknowledged transaction')
                if nested:
                    recovery_point = recovery_points[(case // 4) % len(recovery_points)]
                    record['recovery_point'] = recovery_point
                    second = invoke(root, 'recover', recovery_point)
                    if second.returncode != 91:
                        raise RuntimeError(f'Recovery injection was not reached: {second.stderr}')
                    injected += 1
                recovered = invoke(root, 'recover')
                record['injected_so_far'] = injected
                if recovered.returncode:
                    record.update(ok=False, error=recovered.stderr)
                    failures += 1
                else:
                    observed = json.loads(recovered.stdout)
                    expected = 'after' if point in {'commit_forced', 'data_forced', 'transaction_ended'} else 'before'
                    ok = observed['ack_present'] and observed['state'] == expected
                    lost += not observed['ack_present']
                    failures += not ok
                    record.update(ok=ok, observed=observed, expected=expected)
            ledger.write(json.dumps(record) + '\n')
            ledger.flush()
        summary = {'type': 'summary', 'injected_crashes': injected, 'cases': case,
                   'failed_cases': failures, 'lost_acknowledged_transactions': lost,
                   'oracle': 'independent durable acknowledgement and before/after multi-row atomicity'}
        ledger.write(json.dumps(summary) + '\n')
    print(json.dumps(summary, indent=2))
    return 1 if failures else 0


if __name__ == '__main__':
    raise SystemExit(main())
