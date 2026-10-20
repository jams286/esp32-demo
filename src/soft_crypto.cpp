#include "soft_crypto.h"
#include <math.h>
#include <string.h>

// ---------- tablas ----------
// Se calculan una sola vez al primer uso a partir de las definiciones de FIPS-197
// (inverso en GF(2^8) y transformación afín), así no hay que copiar tablas a mano.

static uint8_t sbox[256], isbox[256];
static uint32_t Te0[256], Te1[256], Te2[256], Te3[256];
static uint32_t Td0[256], Td1[256], Td2[256], Td3[256];
static uint8_t rcon[11];
static bool tablesReady = false;

static uint8_t xtime(uint8_t a) { return (uint8_t)((a << 1) ^ ((a & 0x80) ? 0x1B : 0)); }

static uint8_t gmul(uint8_t a, uint8_t b) {
  uint8_t r = 0;
  while (b) {
    if (b & 1) r ^= a;
    a = xtime(a);
    b >>= 1;
  }
  return r;
}

static uint8_t rotl8(uint8_t x, int n) { return (uint8_t)((x << n) | (x >> (8 - n))); }
static uint32_t ror32(uint32_t x, int n) { return (x >> n) | (x << (32 - n)); }

static void buildTables() {
  if (tablesReady) return;
  for (int x = 0; x < 256; x++) {
    uint8_t inv = 0;  // inverso multiplicativo (0 se queda en 0)
    for (int y = 1; y < 256; y++)
      if (gmul((uint8_t)x, (uint8_t)y) == 1) {
        inv = (uint8_t)y;
        break;
      }
    uint8_t s = inv ^ rotl8(inv, 1) ^ rotl8(inv, 2) ^ rotl8(inv, 3) ^ rotl8(inv, 4) ^ 0x63;
    sbox[x] = s;
    isbox[s] = (uint8_t)x;
  }
  for (int x = 0; x < 256; x++) {
    uint8_t s = sbox[x];
    Te0[x] = ((uint32_t)gmul(s, 2) << 24) | ((uint32_t)s << 16) | ((uint32_t)s << 8) | gmul(s, 3);
    Te1[x] = ror32(Te0[x], 8);
    Te2[x] = ror32(Te0[x], 16);
    Te3[x] = ror32(Te0[x], 24);
    uint8_t si = isbox[x];
    Td0[x] = ((uint32_t)gmul(si, 14) << 24) | ((uint32_t)gmul(si, 9) << 16) | ((uint32_t)gmul(si, 13) << 8) | gmul(si, 11);
    Td1[x] = ror32(Td0[x], 8);
    Td2[x] = ror32(Td0[x], 16);
    Td3[x] = ror32(Td0[x], 24);
  }
  rcon[1] = 1;
  for (int i = 2; i < 11; i++) rcon[i] = xtime(rcon[i - 1]);
  tablesReady = true;
}

// ---------- AES ----------

static inline uint32_t load32(const uint8_t *p) {
  return ((uint32_t)p[0] << 24) | ((uint32_t)p[1] << 16) | ((uint32_t)p[2] << 8) | p[3];
}

static inline void store32(uint8_t *p, uint32_t v) {
  p[0] = v >> 24;
  p[1] = v >> 16;
  p[2] = v >> 8;
  p[3] = v;
}

static uint32_t subWord(uint32_t w) {
  return ((uint32_t)sbox[w >> 24] << 24) | ((uint32_t)sbox[(w >> 16) & 0xFF] << 16) |
         ((uint32_t)sbox[(w >> 8) & 0xFF] << 8) | sbox[w & 0xFF];
}

static bool expandKey(SoftAes *ctx, const uint8_t *key, int keyBits) {
  if (keyBits != 128 && keyBits != 256) return false;
  buildTables();
  int nk = keyBits / 32;
  ctx->rounds = nk + 6;
  int total = 4 * (ctx->rounds + 1);
  for (int i = 0; i < nk; i++) ctx->rk[i] = load32(key + 4 * i);
  for (int i = nk; i < total; i++) {
    uint32_t t = ctx->rk[i - 1];
    if (i % nk == 0) t = subWord((t << 8) | (t >> 24)) ^ ((uint32_t)rcon[i / nk] << 24);
    else if (nk > 6 && i % nk == 4) t = subWord(t);
    ctx->rk[i] = ctx->rk[i - nk] ^ t;
  }
  return true;
}

bool soft_aes_setkey_enc(SoftAes *ctx, const uint8_t *key, int keyBits) {
  return expandKey(ctx, key, keyBits);
}

// Cifrado inverso equivalente: claves de ronda en orden inverso y con InvMixColumns
bool soft_aes_setkey_dec(SoftAes *ctx, const uint8_t *key, int keyBits) {
  SoftAes enc;
  if (!expandKey(&enc, key, keyBits)) return false;
  ctx->rounds = enc.rounds;
  for (int r = 0; r <= enc.rounds; r++) {
    for (int c = 0; c < 4; c++) {
      uint32_t w = enc.rk[4 * (enc.rounds - r) + c];
      if (r > 0 && r < enc.rounds)
        w = Td0[sbox[w >> 24]] ^ Td1[sbox[(w >> 16) & 0xFF]] ^ Td2[sbox[(w >> 8) & 0xFF]] ^ Td3[sbox[w & 0xFF]];
      ctx->rk[4 * r + c] = w;
    }
  }
  return true;
}

void soft_aes_encrypt_block(const SoftAes *ctx, const uint8_t in[16], uint8_t out[16]) {
  const uint32_t *rk = ctx->rk;
  uint32_t s0 = load32(in) ^ rk[0], s1 = load32(in + 4) ^ rk[1];
  uint32_t s2 = load32(in + 8) ^ rk[2], s3 = load32(in + 12) ^ rk[3];
  uint32_t t0, t1, t2, t3;
  for (int r = 1; r < ctx->rounds; r++) {
    rk += 4;
    t0 = Te0[s0 >> 24] ^ Te1[(s1 >> 16) & 0xFF] ^ Te2[(s2 >> 8) & 0xFF] ^ Te3[s3 & 0xFF] ^ rk[0];
    t1 = Te0[s1 >> 24] ^ Te1[(s2 >> 16) & 0xFF] ^ Te2[(s3 >> 8) & 0xFF] ^ Te3[s0 & 0xFF] ^ rk[1];
    t2 = Te0[s2 >> 24] ^ Te1[(s3 >> 16) & 0xFF] ^ Te2[(s0 >> 8) & 0xFF] ^ Te3[s1 & 0xFF] ^ rk[2];
    t3 = Te0[s3 >> 24] ^ Te1[(s0 >> 16) & 0xFF] ^ Te2[(s1 >> 8) & 0xFF] ^ Te3[s2 & 0xFF] ^ rk[3];
    s0 = t0; s1 = t1; s2 = t2; s3 = t3;
  }
  rk += 4;  // última ronda: sin MixColumns
  store32(out,      (((uint32_t)sbox[s0 >> 24] << 24) | ((uint32_t)sbox[(s1 >> 16) & 0xFF] << 16) |
                     ((uint32_t)sbox[(s2 >> 8) & 0xFF] << 8) | sbox[s3 & 0xFF]) ^ rk[0]);
  store32(out + 4,  (((uint32_t)sbox[s1 >> 24] << 24) | ((uint32_t)sbox[(s2 >> 16) & 0xFF] << 16) |
                     ((uint32_t)sbox[(s3 >> 8) & 0xFF] << 8) | sbox[s0 & 0xFF]) ^ rk[1]);
  store32(out + 8,  (((uint32_t)sbox[s2 >> 24] << 24) | ((uint32_t)sbox[(s3 >> 16) & 0xFF] << 16) |
                     ((uint32_t)sbox[(s0 >> 8) & 0xFF] << 8) | sbox[s1 & 0xFF]) ^ rk[2]);
  store32(out + 12, (((uint32_t)sbox[s3 >> 24] << 24) | ((uint32_t)sbox[(s0 >> 16) & 0xFF] << 16) |
                     ((uint32_t)sbox[(s1 >> 8) & 0xFF] << 8) | sbox[s2 & 0xFF]) ^ rk[3]);
}

void soft_aes_decrypt_block(const SoftAes *ctx, const uint8_t in[16], uint8_t out[16]) {
  const uint32_t *rk = ctx->rk;
  uint32_t s0 = load32(in) ^ rk[0], s1 = load32(in + 4) ^ rk[1];
  uint32_t s2 = load32(in + 8) ^ rk[2], s3 = load32(in + 12) ^ rk[3];
  uint32_t t0, t1, t2, t3;
  for (int r = 1; r < ctx->rounds; r++) {
    rk += 4;
    t0 = Td0[s0 >> 24] ^ Td1[(s3 >> 16) & 0xFF] ^ Td2[(s2 >> 8) & 0xFF] ^ Td3[s1 & 0xFF] ^ rk[0];
    t1 = Td0[s1 >> 24] ^ Td1[(s0 >> 16) & 0xFF] ^ Td2[(s3 >> 8) & 0xFF] ^ Td3[s2 & 0xFF] ^ rk[1];
    t2 = Td0[s2 >> 24] ^ Td1[(s1 >> 16) & 0xFF] ^ Td2[(s0 >> 8) & 0xFF] ^ Td3[s3 & 0xFF] ^ rk[2];
    t3 = Td0[s3 >> 24] ^ Td1[(s2 >> 16) & 0xFF] ^ Td2[(s1 >> 8) & 0xFF] ^ Td3[s0 & 0xFF] ^ rk[3];
    s0 = t0; s1 = t1; s2 = t2; s3 = t3;
  }
  rk += 4;
  store32(out,      (((uint32_t)isbox[s0 >> 24] << 24) | ((uint32_t)isbox[(s3 >> 16) & 0xFF] << 16) |
                     ((uint32_t)isbox[(s2 >> 8) & 0xFF] << 8) | isbox[s1 & 0xFF]) ^ rk[0]);
  store32(out + 4,  (((uint32_t)isbox[s1 >> 24] << 24) | ((uint32_t)isbox[(s0 >> 16) & 0xFF] << 16) |
                     ((uint32_t)isbox[(s3 >> 8) & 0xFF] << 8) | isbox[s2 & 0xFF]) ^ rk[1]);
  store32(out + 8,  (((uint32_t)isbox[s2 >> 24] << 24) | ((uint32_t)isbox[(s1 >> 16) & 0xFF] << 16) |
                     ((uint32_t)isbox[(s0 >> 8) & 0xFF] << 8) | isbox[s3 & 0xFF]) ^ rk[2]);
  store32(out + 12, (((uint32_t)isbox[s3 >> 24] << 24) | ((uint32_t)isbox[(s2 >> 16) & 0xFF] << 16) |
                     ((uint32_t)isbox[(s1 >> 8) & 0xFF] << 8) | isbox[s0 & 0xFF]) ^ rk[3]);
}

void soft_aes_cbc_encrypt(const SoftAes *ctx, size_t len, uint8_t iv[16], const uint8_t *in, uint8_t *out) {
  uint8_t blk[16];
  for (size_t off = 0; off + 16 <= len; off += 16) {
    for (int i = 0; i < 16; i++) blk[i] = in[off + i] ^ iv[i];
    soft_aes_encrypt_block(ctx, blk, out + off);
    memcpy(iv, out + off, 16);
  }
}

void soft_aes_cbc_decrypt(const SoftAes *ctx, size_t len, uint8_t iv[16], const uint8_t *in, uint8_t *out) {
  uint8_t blk[16], next[16];
  for (size_t off = 0; off + 16 <= len; off += 16) {
    memcpy(next, in + off, 16);  // por si in y out son el mismo búfer
    soft_aes_decrypt_block(ctx, in + off, blk);
    for (int i = 0; i < 16; i++) out[off + i] = blk[i] ^ iv[i];
    memcpy(iv, next, 16);
  }
}

// ---------- SHA-256 ----------
// Constantes de FIPS 180-4: fracciones de las raíces cuadradas (valores iniciales) y
// cúbicas (K) de los primeros primos. Se calculan una vez; la autoverificación lo comprueba.

static uint32_t shaK[64], shaH0[8];
static bool shaReady = false;

static void buildSha() {
  if (shaReady) return;
  int found = 0;
  for (int n = 2; found < 64; n++) {
    bool prime = true;
    for (int d = 2; d * d <= n; d++)
      if (n % d == 0) {
        prime = false;
        break;
      }
    if (!prime) continue;
    if (found < 8) {
      double r = sqrt((double)n);
      shaH0[found] = (uint32_t)((r - floor(r)) * 4294967296.0);
    }
    double c = cbrt((double)n);
    shaK[found] = (uint32_t)((c - floor(c)) * 4294967296.0);
    found++;
  }
  shaReady = true;
}

static inline uint32_t rotr(uint32_t x, int n) { return (x >> n) | (x << (32 - n)); }

static void shaBlock(SoftSha256 *ctx, const uint8_t *p) {
  uint32_t w[64];
  for (int i = 0; i < 16; i++) w[i] = load32(p + 4 * i);
  for (int i = 16; i < 64; i++) {
    uint32_t s0 = rotr(w[i - 15], 7) ^ rotr(w[i - 15], 18) ^ (w[i - 15] >> 3);
    uint32_t s1 = rotr(w[i - 2], 17) ^ rotr(w[i - 2], 19) ^ (w[i - 2] >> 10);
    w[i] = w[i - 16] + s0 + w[i - 7] + s1;
  }
  uint32_t a = ctx->h[0], b = ctx->h[1], c = ctx->h[2], d = ctx->h[3];
  uint32_t e = ctx->h[4], f = ctx->h[5], g = ctx->h[6], h = ctx->h[7];
  for (int i = 0; i < 64; i++) {
    uint32_t S1 = rotr(e, 6) ^ rotr(e, 11) ^ rotr(e, 25);
    uint32_t ch = (e & f) ^ (~e & g);
    uint32_t t1 = h + S1 + ch + shaK[i] + w[i];
    uint32_t S0 = rotr(a, 2) ^ rotr(a, 13) ^ rotr(a, 22);
    uint32_t maj = (a & b) ^ (a & c) ^ (b & c);
    uint32_t t2 = S0 + maj;
    h = g; g = f; f = e; e = d + t1; d = c; c = b; b = a; a = t1 + t2;
  }
  ctx->h[0] += a; ctx->h[1] += b; ctx->h[2] += c; ctx->h[3] += d;
  ctx->h[4] += e; ctx->h[5] += f; ctx->h[6] += g; ctx->h[7] += h;
}

void soft_sha256_init(SoftSha256 *ctx) {
  buildSha();
  memcpy(ctx->h, shaH0, sizeof(shaH0));
  ctx->bufLen = 0;
  ctx->totalLen = 0;
}

void soft_sha256_update(SoftSha256 *ctx, const uint8_t *data, size_t len) {
  ctx->totalLen += len;
  while (len) {
    if (ctx->bufLen == 0 && len >= 64) {
      shaBlock(ctx, data);
      data += 64;
      len -= 64;
      continue;
    }
    size_t room = 64 - ctx->bufLen;
    size_t n = len < room ? len : room;
    memcpy(ctx->buf + ctx->bufLen, data, n);
    ctx->bufLen += n;
    data += n;
    len -= n;
    if (ctx->bufLen == 64) {
      shaBlock(ctx, ctx->buf);
      ctx->bufLen = 0;
    }
  }
}

void soft_sha256_final(SoftSha256 *ctx, uint8_t out[32]) {
  uint64_t bits = ctx->totalLen * 8;
  uint8_t pad = 0x80, zero = 0;
  soft_sha256_update(ctx, &pad, 1);
  while (ctx->bufLen != 56) soft_sha256_update(ctx, &zero, 1);
  uint8_t lenBytes[8];
  for (int i = 0; i < 8; i++) lenBytes[i] = (uint8_t)(bits >> (56 - 8 * i));
  soft_sha256_update(ctx, lenBytes, 8);
  for (int i = 0; i < 8; i++) store32(out + 4 * i, ctx->h[i]);
}
