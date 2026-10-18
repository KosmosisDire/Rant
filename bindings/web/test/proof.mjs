// The passphrase proof against SHA-256 and RFC 4231's HMAC vectors.   node bindings/web/test/proof.mjs

import assert from "node:assert/strict";
import { sha256, hmacSha256, passphraseProof } from "../dist/proof.js";

const hex = (b) => [...b].map((x) => x.toString(16).padStart(2, "0")).join("");
const bytes = (s) => new TextEncoder().encode(s);
let failed = 0;
function check(name, f) {
    try { f(); console.log(`ok   ${name}`); } catch (e) { failed++; console.log(`FAIL ${name}: ${e.message}`); }
}

check("sha256 of abc", () => assert.equal(hex(sha256(bytes("abc"))), "ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad"));
check("sha256 of nothing", () => assert.equal(hex(sha256(new Uint8Array(0))), "e3b0c44298fc1c149afbf4c8996fb92427ae41e4649b934ca495991b7852b855"));
check("sha256 across two blocks", () => assert.equal(hex(sha256(bytes("abcdbcdecdefdefgefghfghighijhijkijkljklmklmnlmnomnopnopq"))),
    "248d6a61d20638b8e5c026930c3e6039a33ce45964ff2167f6ecedd419db06c1"));
check("sha256 of a million a", () => assert.equal(hex(sha256(new Uint8Array(1000000).fill(0x61))),
    "cdc76e5c9914fb9281a1c7e284d73e67f1809a48a497200e046d39ccc7112cd0"));
check("hmac, RFC 4231 case 2", () => assert.equal(hex(hmacSha256(bytes("Jefe"), bytes("what do ya want for nothing?"))),
    "5bdcc146bf60754e6a042426089575c75a003f089d2739839dec58b964ec3843"));
check("hmac with a key past a block, case 6", () => assert.equal(
    hex(hmacSha256(new Uint8Array(131).fill(0xaa), bytes("Test Using Larger Than Block-Size Key - Hash Key First"))),
    "60e431591ee0b67f0d8a26aacbf5b77f8e0bc6213728c5140546040f0ee37f54"));
check("the proof is the hmac of the nonce in hex", () => assert.equal(passphraseProof("Jefe", "what do ya want for nothing?"),
    "5bdcc146bf60754e6a042426089575c75a003f089d2739839dec58b964ec3843"));
process.exit(failed ? 1 : 0);
