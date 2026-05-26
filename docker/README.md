# Device-agent Docker Fleet

S2 provides the small-scale fleet wiring used by M3-Beta-Scale.

```bash
docker build -f docker/Dockerfile.tracker -t opentracker:s2 .
docker build -f docker/Dockerfile.p2p-runner -t runner:s2 .
bash bin/fleet-compose-gen.sh --size=10 > /tmp/fleet-10.yml
bash scripts/fleet-smoke.sh --size=10
```

`Dockerfile.tracker` packages erdgeist opentracker from Alpine and exposes TCP/UDP `6969`.
TCP announce requests are passed through `socat -v` so `docker compose logs tracker | grep /announce`
can be used as fleet-smoke evidence.

`fleet-smoke.sh` preloads one seed directory with the payload, starts that runner in seeding mode,
then starts the generated fleet with no web seed URL. A successful smoke therefore proves tracker
discovery plus peer download, SHA verification, and announce logging in one loop.
Set `FLEET_SMOKE_KEEP=1` to keep the temporary compose project for debugging failed runs.

`fleet-ramp.sh` is intentionally a skeleton for S5. S5 owns soak duration, resource thresholds,
and 30/60/100 ramp evidence.
