#!/usr/bin/env python3
"""Run under build-queue.sh: prove each fanout regression fails without its guard.
Restores, touches and rebuilds production source after EVERY mutation.
"""
import os
from pathlib import Path
import subprocess

root = Path(__file__).resolve().parents[2]
os.chdir(root)
fanout = Path('libs/core/capture/CaptureFanout.cpp')
writer = Path('libs/core/capture/RawCapture.cpp')
app = Path('libs/core/capture/CaptureApp.cpp')
checks = [
    ('writer publication', writer, 'if (m_config.onBlock)', 'if (false && m_config.onBlock)', 'Fixture.WriterBytesPositionsRotationAndShutdown'),
    ('exclusive resume', fanout, 'begin = std::next(found)', 'begin = found', 'Fixture.ResumeExcludesCursorAndIncludesEverySuccessor'),
    ('explicit gap', fanout, '{"type", "gap"}', '{"type", "tip"}', 'Fixture.TimeAndByteEvictionReturnExplicitJournalBoundary'),
    ('time retention', fanout, 'now - p.ring.front().time >= config.retention.count() * 1000000', 'false', 'Fixture.TimeAndByteEvictionReturnExplicitJournalBoundary'),
    ('byte retention', fanout, 'p.ringBytes > config.ringBytes', 'false', 'Fixture.TimeAndByteEvictionReturnExplicitJournalBoundary'),
    ('client backpressure', fanout, 'cost > config.clientBytes || c.bytes > config.clientBytes - cost', 'false', 'Fixture.SlowClientCannotBlockWriterOrHealthyClient'),
    ('ingress gap', fanout, 'invalidate(i, p); p.seenEpoch = epoch;', 'p.seenEpoch = epoch;', 'Fixture.IngressOverflowInvalidatesResumeWithoutFailingWriter'),
    ('rate limit', fanout, 'const bool limited = p.lastResnapshot &&', 'const bool limited = false && p.lastResnapshot &&', 'Fixture.ResnapshotRoutesAndRateLimitsAcrossClientsPerProduct'),
    ('resnapshot route', fanout, 'if (resnapshot) resnapshot(product);', 'if (false && resnapshot) resnapshot(product);', 'Fixture.ResnapshotRoutesAndRateLimitsAcrossClientsPerProduct'),
    ('private directory', fanout, '(st.st_mode & 0777) != 0700', 'false', 'Fixture.SocketSafetyOwnershipAndFailedStartupNeverDeletesFiles'),
    ('client shutdown', fanout, 'drop(c, "shutdown");', '(void)c;', 'Fixture.WriterBytesPositionsRotationAndShutdown'),
    ('no-client retention', fanout, 'auto& p = *products[i];\n            const auto epoch', 'auto& p = *products[i];\n            if (connected->value() == 0) continue;\n            const auto epoch', 'Fixture.IdleAllocatesNoPayloadAndRetentionIsIndependentOfClients'),
    ('application route', app, 'feeds->requestResnapshot(product);', '(void)product;', 'CaptureApplication.FanoutResnapshotReachesOnlyRequestedEngine'),
]

def build(target):
    result = subprocess.run(['cmake', '--build', '--preset', 'mac-clang', '-j', '4', '--target', target], text=True, stdout=subprocess.PIPE, stderr=subprocess.STDOUT)
    if result.returncode:
        print(result.stdout, flush=True)
        raise RuntimeError('mutation build failed')

for label, path, old, new, case in checks:
    original = path.read_text()
    if original.count(old) != 1:
        raise RuntimeError(f'{label}: expected one replacement, found {original.count(old)}')
    target = 'test_capture_app' if case.startswith('CaptureApplication.') else 'test_capture_fanout'
    try:
        path.write_text(original.replace(old, new)); path.touch()
        build(target)
        result = subprocess.run([f'build/mac-clang/tests/capture/{target}', f'--gtest_filter={case}'], text=True, stdout=subprocess.PIPE, stderr=subprocess.STDOUT, timeout=45)
        if result.returncode == 0 or '[  FAILED  ]' not in result.stdout:
            print(result.stdout, flush=True)
            raise RuntimeError(f'{label}: mutation did not fail its regression')
        print(f'FAIL-WITHOUT confirmed: {label} ({case})', flush=True)
    finally:
        path.write_text(original); path.touch(); build(target)
    result = subprocess.run([f'build/mac-clang/tests/capture/{target}', f'--gtest_filter={case}'], text=True, stdout=subprocess.PIPE, stderr=subprocess.STDOUT, timeout=45)
    if result.returncode:
        print(result.stdout, flush=True)
        raise RuntimeError(f'{label}: restored regression failed')
    print(f'RESTORED passes: {label}', flush=True)
print(f'All {len(checks)} fail-without checks passed; all sources restored, touched and rebuilt.', flush=True)
