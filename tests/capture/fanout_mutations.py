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
durable_notification = '    m_durable = std::pair{entry.ordinal, m_count - 1};\n    if (m_config.onJournal) m_config.onJournal({JournalEventKind::Durable,\n        m_metadata.at("run_id").get_ref<const std::string&>(), entry.ordinal, m_count - 1, true, {}});\n'
flush_io = '    write(header); write(compressed);\n    if (!m_file.flush()) fail("block flush failed");\n    ++m_stats.blocks;\n    if (m_config.fsyncBlocks && m_stats.blocks % m_config.fsyncBlocks == 0) sync();\n    m_index.push_back(entry);\n'
checks = [
    ('durable after physical flush', writer, flush_io + durable_notification, durable_notification + flush_io, 'Fixture.ProvisionalArrivesBeforeFlushAndDurableFollows'),
    ('writer publication', writer, '        m_config.onJournal({JournalEventKind::Record,', '        if (false) m_config.onJournal({JournalEventKind::Record,', 'Fixture.WriterBytesPositionsRotationAndShutdown'),
    ('provisional before flush', writer, '        m_config.onJournal({JournalEventKind::Record,', '        if (false) m_config.onJournal({JournalEventKind::Record,', 'Fixture.ProvisionalArrivesBeforeFlushAndDurableFollows'),
    ('durable notification', writer, 'if (m_config.onJournal) m_config.onJournal({JournalEventKind::Durable,', 'if (false && m_config.onJournal) m_config.onJournal({JournalEventKind::Durable,', 'Fixture.DurableProvisionalOrderAndResumeWatermark'),
    ('flush failure retract', writer, '} catch (...) { retract(); throw; }\nvoid Writer::seal()', '} catch (...) { throw; }\nvoid Writer::seal()', 'Fixture.WriteFailureRetractsAndConsumerDiscardsSuffix'),
    ('retract ring suffix', fanout, 'while (!p.ring.empty() && (!p.durable ||', 'while (false && !p.ring.empty() && (!p.durable ||', 'Fixture.WriteFailureRetractsAndConsumerDiscardsSuffix'),
    ('position non-reuse', writer, 'if (m_appendedBlock) m_ordinal =', 'if (false && m_appendedBlock) m_ordinal =', 'Fixture.RetractionBeforeFirstFlushIsNullAndPositionsAreNotReused'),
    ('session fault retract', Path('libs/core/capture/CaptureSession.cpp'), 'if (writer) writer->retract(); failed = true;', 'failed = true;', 'Fixture.SessionFaultRetractsBeforeWaitingForClose'),
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
    ('setup failure keeps application alive', fanout, 'catch (const std::exception& e) { unavailable(e); }', 'catch (const std::exception& e) { throw; }', 'CaptureApplication.FanoutUnavailableKeepsJournalingAndRecovers'),
    ('setup retry', fanout, 'if (listener < 0 && config.nowNs() >= nextRetry) setup();', 'if (false && listener < 0 && config.nowNs() >= nextRetry) setup();', 'Fixture.SetupFailureRetriesWithBackoffWhileWriterContinues'),
    ('exponential retry delay', fanout, 'retryMs = std::min<int64_t>(retryMs * 2, 600000);', 'retryMs = 30000;', 'Fixture.SetupFailureRetriesWithBackoffWhileWriterContinues'),
    ('retry delay ceiling', fanout, 'retryMs = std::min<int64_t>(retryMs * 2, 600000);', 'retryMs *= 2;', 'Fixture.SetupRetryBackoffCapsAtTenMinutesAndStopsWhileDown'),
    ('engine cooldown alignment', fanout, 'now - *p.lastResnapshot < config.resnapshotInterval.count() * 1000000', 'now - *p.lastResnapshot < 10000000000LL', 'Fixture.ResnapshotCooldownAndGlobalCapReportForwarding'),
    ('global resnapshot cap', fanout, 'globalResnapshots.size() >= 3', 'false', 'Fixture.ResnapshotCooldownAndGlobalCapReportForwarding'),
    ('forwarded reply semantics', fanout, ': "forwarded"', ': "accepted"', 'Fixture.ResnapshotCooldownAndGlobalCapReportForwarding'),
    ('malformed ingress product isolation', fanout, 'invalidate(i, p, "malformed_ingress");', 'throw std::runtime_error("invalid fanout record");', 'Fixture.MalformedIngressIsolatesOnlyItsProduct'),
    ('malformed ingress clears ring', fanout, 'invalidate(i, p, "malformed_ingress");', 'for (auto& c : clients) if (c.product == int(i)) drop(c, "malformed_ingress");', 'Fixture.MalformedIngressIsolatesOnlyItsProduct'),
    ('malformed ingress logs once', fanout, 'if (!p.malformedLogged)', 'if (true)', 'Fixture.MalformedIngressIsolatesOnlyItsProduct'),
    ('fanout down alert enabled', Path('ops/monitoring/grafana/provisioning/alerting/rules.yaml'), 'uid: sentinel-capture-fanout-down', 'uid: disabled-capture-fanout-down', 'CaptureFanoutAlert.UnavailableForFiveMinutesAlertsWithoutPagingOnAbsence'),

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
        result = subprocess.run([f'build/mac-clang/tests/capture/{target}', f'--gtest_filter={case}'], text=True, stdout=subprocess.PIPE, stderr=subprocess.STDOUT, timeout=90)
        if result.returncode == 0 or '[  FAILED  ]' not in result.stdout:
            print(result.stdout, flush=True)
            raise RuntimeError(f'{label}: mutation did not fail its regression')
        print(f'FAIL-WITHOUT confirmed: {label} ({case})', flush=True)
    finally:
        path.write_text(original); path.touch(); build(target)
    result = subprocess.run([f'build/mac-clang/tests/capture/{target}', f'--gtest_filter={case}'], text=True, stdout=subprocess.PIPE, stderr=subprocess.STDOUT, timeout=90)
    if result.returncode:
        print(result.stdout, flush=True)
        raise RuntimeError(f'{label}: restored regression failed')
    print(f'RESTORED passes: {label}', flush=True)
print(f'All {len(checks)} fail-without checks passed; all sources restored, touched and rebuilt.', flush=True)
