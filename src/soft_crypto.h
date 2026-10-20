// AES (128/256 bits, modo CBC) y SHA-256 por software, escritos desde cero según
// FIPS-197 y FIPS 180-4. Sirven de referencia para comparar contra el acelerador por hardware.
// El AES usa tablas (T-tables), como el camino por software de mbedTLS sin acelerador.

#pragma once
#include <stddef.h>
#include <stdint.h>

struct SoftAes {
  uint32_t rk[60];  // claves de ronda (hasta 14 rondas + 1, de 4 palabras)
  int rounds;       // 10 (AES-128) o 14 (AES-256)
};

// keyBits: 128 o 256. Devuelven false si el tamaño no es válido
bool soft_aes_setkey_enc(SoftAes *ctx, const uint8_t *key, int keyBits);
bool soft_aes_setkey_dec(SoftAes *ctx, const uint8_t *key, int keyBits);

void soft_aes_encrypt_block(const SoftAes *ctx, const uint8_t in[16], uint8_t out[16]);
void soft_aes_decrypt_block(const SoftAes *ctx, const uint8_t in[16], uint8_t out[16]);

// len debe ser múltiplo de 16. El IV se actualiza (queda con el último bloque), como en mbedTLS
void soft_aes_cbc_encrypt(const SoftAes *ctx, size_t len, uint8_t iv[16], const uint8_t *in, uint8_t *out);
void soft_aes_cbc_decrypt(const SoftAes *ctx, size_t len, uint8_t iv[16], const uint8_t *in, uint8_t *out);

struct SoftSha256 {
  uint32_t h[8];
  uint8_t buf[64];
  uint32_t bufLen;
  uint64_t totalLen;  // en bytes
};

void soft_sha256_init(SoftSha256 *ctx);
void soft_sha256_update(SoftSha256 *ctx, const uint8_t *data, size_t len);
void soft_sha256_final(SoftSha256 *ctx, uint8_t out[32]);
