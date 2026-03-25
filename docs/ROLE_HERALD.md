# Herald — Mesh Infrastructure

## Role

The Herald is the collective's internal communication infrastructure. It runs and manages the MQTT broker that every other agent connects through. No agent communicates directly with another — all traffic flows through the Herald.

The Herald is pure infrastructure. It does not execute programs, hold data, vote on consensus, or participate in the collective's decision-making. It ensures that messages are delivered, credentials are valid, and the mesh is healthy.

## Responsibilities

### Broker Management
- Supervise the MQTT broker process (Mosquitto or EMQX)
- Restart the broker on crash and alert the mesh via `nml/herald/status`
- Configure persistence paths, log rotation, and connection limits
- Manage TLS certificates for encrypted agent connections

### Credentials
- Issue MQTT client credentials (username/password or client certificates) to agents registering with the Sentient
- Revoke credentials when the Enforcer quarantines or blacklists a node — revocation takes effect immediately on the next CONNECT attempt
- Rotate credentials on schedule for long-running agents

### ACL Configuration
- Define which agents may publish to which topics
- Enforce topic-level access control: workers cannot publish to `nml/program`, only sentients and architects can
- Apply Enforcer policy decisions as broker-level ACL rules

### Health Monitoring
- Subscribe to `$SYS/broker/#` metrics published by the broker (connected clients, message rates, queue depths, bytes in/out)
- Publish a health summary to `nml/herald/status` on a regular interval
- Alert on broker degradation: connection refusals, queue buildup, high message drop rates

### Federation
- Bridge to Herald instances at other sites for multi-site collectives
- Configure MQTT bridge connections: which topics flow between sites, in which direction

## Topic Responsibilities

The Herald does not publish to agent topics. It only manages broker configuration and publishes its own status:

| Topic | Direction | Purpose |
|-------|-----------|---------|
| `nml/herald/status` | Publish | Broker health summary |
| `$SYS/broker/#` | Subscribe | Broker metrics (internal) |

## Relationship with Enforcer

The Enforcer monitors agent behavior and decides *who* should be removed from the mesh. The Herald carries out the removal by revoking credentials and applying ACL blocks. They are intentionally separated:

- Enforcer: behavioral policy (watches what agents do)
- Herald: access control (controls what agents can connect)

The Enforcer cannot directly disconnect an agent — it requests revocation from the Herald. The Herald cannot decide who to revoke — it only acts on Enforcer instructions.

## Relationship with Sentient

The Sentient acts as the trust anchor for new agents joining the collective. When a new agent registers:

1. Agent contacts the Sentient with its identity payload
2. Sentient verifies the identity and decides whether to admit the agent
3. Sentient requests credentials from the Herald
4. Herald issues credentials and returns them to the Sentient
5. Sentient delivers credentials to the new agent
6. Agent connects to Herald with those credentials

## Deployment

The Herald should be deployed on a machine with a stable, reachable address — the same machine as the primary Sentient for single-site deployments, or a dedicated VM/server for multi-site.

For high availability, EMQX supports clustering. Multiple Herald instances can form a cluster; agents connect to any node in the cluster.

## Implementation Notes (C99)

The C99 `herald_agent` does not implement the MQTT broker itself. It:

1. Generates a `mosquitto.conf` from its configuration
2. Exec's into Mosquitto (or launches it as a supervised child process)
3. Monitors Mosquitto via its `$SYS/` topics
4. Exposes a local management API for credential issuance and ACL updates

This keeps the Herald implementation small and delegates broker functionality to a proven, production-ready broker.

## TODO

- [ ] Generate `mosquitto.conf` from command-line configuration
- [ ] Supervise Mosquitto child process, restart on crash
- [ ] Subscribe to `$SYS/broker/#` and publish health summary
- [ ] REST endpoint for credential issuance (called by Sentient)
- [ ] REST endpoint for credential revocation (called by Enforcer)
- [ ] ACL file management — add/remove entries without broker restart
- [ ] TLS certificate provisioning
- [ ] MQTT bridge configuration for multi-site federation
