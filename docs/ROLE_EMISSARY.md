# Emissary — External Boundary

## Role

The Emissary is the collective's boundary role. It is the single point of contact between the internal mesh and everything outside it — human operators, external systems, other collectives, and downstream consumers.

No internal agent is directly reachable from outside the collective. All external traffic enters and exits through the Emissary. Internal agents never expose themselves to the outside world.

## Responsibilities

### Human Interface
- REST API for operators and administrators: submit programs, query state, view consensus results, manage the collective
- Dashboard serving: the collective's web UI is hosted by the Emissary, not individual agents
- Authentication: human users authenticate at the Emissary before any request reaches the mesh

### Inter-Collective Federation
- Connect to other collectives' Emissaries to share programs, results, and data
- Translate between collectives that may use different protocol versions or topic structures
- Forward committed results to peer collectives that have subscribed to this collective's output
- Receive programs and data submissions from peer collectives after verifying their identity

### Data Ingestion
- Accept data from external sources: databases, APIs, sensors, file uploads
- Validate and normalize incoming data before forwarding to the Sentient for quarantine processing
- Rate-limit and authenticate external data producers

### Result Export
- Deliver committed Oracle results to downstream consumers
- Support push delivery via webhooks (HTTP POST to registered URLs)
- Support pull delivery via REST endpoints
- Filter exports by program hash, time range, confidence level, or custom criteria

### Authentication Gateway
- All external parties authenticate at the Emissary
- The Emissary translates external credentials (API keys, OAuth tokens, mTLS certificates) into internal MQTT credentials
- Unauthorized requests never reach internal agents

## API Endpoints

### Submission

| Endpoint | Method | Purpose |
|----------|--------|---------|
| `/submit` | POST | Submit a program to the collective |
| `/data/submit` | POST | Submit data for quarantine processing |
| `/consensus` | POST | Request consensus for a program hash |

### Query

| Endpoint | Method | Purpose |
|----------|--------|---------|
| `/status` | GET | Collective health and peer summary |
| `/results` | GET | Committed results (filterable) |
| `/peers` | GET | Active agent list |
| `/dashboard` | GET | Web dashboard |

### Federation

| Endpoint | Method | Purpose |
|----------|--------|---------|
| `/federation/register` | POST | Register a peer collective |
| `/federation/receive` | POST | Receive a program or result from a peer collective |
| `/federation/peers` | GET | Known peer collectives |

### Export

| Endpoint | Method | Purpose |
|----------|--------|---------|
| `/export/webhook` | POST | Register a webhook for result delivery |
| `/export/webhook` | DELETE | Remove a webhook |
| `/export/stream` | GET | Server-sent events stream of committed results |

## Relationship with the Mesh

The Emissary is a full MQTT client connected to the Herald. It:

- Subscribes to `nml/committed/#` to receive results for export
- Publishes to `nml/submit` to forward programs from external sources
- Publishes to `nml/data/submit` to forward external data submissions
- Subscribes to `nml/herald/status` for collective health

It does not subscribe to internal coordination topics (`nml/program`, `nml/result/#`, `nml/heartbeat/#`) — those are for agent-to-agent coordination, not external consumption.

## Inter-Collective Protocol

When two collectives federate, their Emissaries exchange:

1. **Handshake**: mutual identity verification (Ed25519 signed introduction)
2. **Capability exchange**: what programs, data domains, and result types each collective produces
3. **Subscription**: each Emissary declares which result types it wants to receive
4. **Forwarding**: committed results matching subscriptions are forwarded between Emissaries

Forwarded results carry a provenance chain: the originating collective is recorded alongside the result so the receiving collective knows where data came from.

## Security

- The Emissary is the only role with an exposed public endpoint
- It must be placed behind a TLS-terminating reverse proxy for production deployments
- Rate limiting applies to all external endpoints
- Requests are authenticated before any internal topic is published to
- The internal mesh is never exposed — even if the Emissary is compromised, the Herald's ACLs prevent direct agent access

## TODO

- [ ] REST API server (aiohttp or C HTTP server)
- [ ] Authentication middleware (API keys, JWT)
- [ ] Dashboard serving
- [ ] MQTT client connected to Herald
- [ ] Webhook registry and delivery engine
- [ ] Server-sent events stream for results
- [ ] Federation handshake and peer collective registry
- [ ] Inbound data normalization and forwarding to Sentient
- [ ] Rate limiting and request logging
