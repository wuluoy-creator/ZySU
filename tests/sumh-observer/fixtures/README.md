# Synthetic host fixtures

Every JSON file here is generated test data. None is an Android observation or evidence that SUMH passed a device test. The build identity deliberately says synthetic-host-fixture-not-a-device-build-v1.

Regenerate using: python tests/sumh-observer/host/synthetic.py. Request bytes have no appended newline; changing whitespace changes the request digest and requires regenerating the associated samples and expectation file.

The examples exercise three independently declared roles, a delegated FD observation, opaque filename bytes, nanosecond/inode values beyond double precision, an explicit ENOENT check, and supplied-but-unverified configuration association. The three actor records deliberately contain simple synthetic values; they do not model real Android DAC or SELinux defaults.
