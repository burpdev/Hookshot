Monocypher 4.0.2, from https://monocypher.org (release monocypher-4.0.2.tar.gz, sha256
38d07179738c0c90677dba3ceb7a7b8496bcfea758ba1a53e803fed30ae0879c), unmodified: src/monocypher.c, src/monocypher.h and
LICENCE.md. Dual-licensed BSD-2-Clause or CC0-1.0 (LICENCE.md); used here under the BSD licence, which is compatible
with the GPL.

Compiled into the runtime by nx2/build.py. Used by host/coop_proto.c alone, for co-op's wire format (docs/coop.md):
keyed BLAKE2b (RFC 7693) for a room's keys and the datagram tags, XChaCha20-Poly1305 for the players' sealed messages,
and EdDSA over Curve25519 with BLAKE2b for what one player signs.
