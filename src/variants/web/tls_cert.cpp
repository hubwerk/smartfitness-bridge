#include "tls_cert.h"

#include <mbedtls/ctr_drbg.h>
#include <mbedtls/ecp.h>
#include <mbedtls/entropy.h>
#include <mbedtls/oid.h>
#include <mbedtls/pk.h>
#include <mbedtls/x509_crt.h>

#include <algorithm>
#include <cstdio>
#include <vector>

#ifdef ARDUINO
#include <Arduino.h>
#include <Preferences.h>
#endif

namespace web {
namespace {

constexpr const char* kPersonalization = "sfbridge-tls";
constexpr std::size_t kSerialBytes = 16;

// Owns the mbedTLS objects of one generation run.
struct Generator {
    mbedtls_entropy_context entropy;
    mbedtls_ctr_drbg_context drbg;
    mbedtls_pk_context key;
    mbedtls_x509write_cert certificate;
    mbedtls_mpi serial;

    Generator() {
        mbedtls_entropy_init(&entropy);
        mbedtls_ctr_drbg_init(&drbg);
        mbedtls_pk_init(&key);
        mbedtls_x509write_crt_init(&certificate);
        mbedtls_mpi_init(&serial);
    }
    ~Generator() {
        mbedtls_mpi_free(&serial);
        mbedtls_x509write_crt_free(&certificate);
        mbedtls_pk_free(&key);
        mbedtls_ctr_drbg_free(&drbg);
        mbedtls_entropy_free(&entropy);
    }
    Generator(const Generator&) = delete;
    Generator& operator=(const Generator&) = delete;
};

bool failed(int code, const char* step, std::string& error) {
    if (code == 0) return false;
    char buffer[64];
    std::snprintf(buffer, sizeof(buffer), "%s failed (-0x%04x)", step, static_cast<unsigned>(-code));
    error = buffer;
    return true;
}

// SubjectAltName: SEQUENCE { dNSName [2] name, iPAddress [7] address }.
std::vector<unsigned char> subjectAltName(const std::string& dnsName, const std::array<std::uint8_t, 4>& ip) {
    std::vector<unsigned char> names;
    names.push_back(0x82);
    names.push_back(static_cast<unsigned char>(dnsName.size()));
    names.insert(names.end(), dnsName.begin(), dnsName.end());
    names.push_back(0x87);
    names.push_back(static_cast<unsigned char>(ip.size()));
    names.insert(names.end(), ip.begin(), ip.end());
    std::vector<unsigned char> sequence{0x30, static_cast<unsigned char>(names.size())};
    sequence.insert(sequence.end(), names.begin(), names.end());
    return sequence;
}

}  // namespace

bool generateTlsIdentity(const std::string& dnsName, const std::array<std::uint8_t, 4>& ipAddress,
                         TlsIdentity& identity, std::string& error) {
    if (dnsName.empty() || dnsName.size() > 64) {
        error = "invalid DNS name";
        return false;
    }
    Generator g;
    if (failed(mbedtls_ctr_drbg_seed(&g.drbg, mbedtls_entropy_func, &g.entropy,
                                     reinterpret_cast<const unsigned char*>(kPersonalization),
                                     std::char_traits<char>::length(kPersonalization)),
               "DRBG seed", error) ||
        failed(mbedtls_pk_setup(&g.key, mbedtls_pk_info_from_type(MBEDTLS_PK_ECKEY)), "key setup", error) ||
        failed(mbedtls_ecp_gen_key(MBEDTLS_ECP_DP_SECP256R1, mbedtls_pk_ec(g.key), mbedtls_ctr_drbg_random, &g.drbg),
               "key generation", error))
        return false;

    // Random positive serial so browsers never see two certificates with the same issuer and serial.
    unsigned char serial[kSerialBytes];
    if (failed(mbedtls_ctr_drbg_random(&g.drbg, serial, sizeof(serial)), "serial", error)) return false;
    serial[0] = static_cast<unsigned char>((serial[0] & 0x7f) | 0x01);
    if (failed(mbedtls_mpi_read_binary(&g.serial, serial, sizeof(serial)), "serial", error)) return false;

    const std::string name = "CN=" + dnsName + ",O=SmartFitness Bridge";
    const std::vector<unsigned char> altNames = subjectAltName(dnsName, ipAddress);
    static const unsigned char kServerAuth[] = {0x30, 0x0a, 0x06, 0x08, 0x2b, 0x06, 0x01, 0x05, 0x05, 0x07, 0x03, 0x01};
    mbedtls_x509write_cert* crt = &g.certificate;
    mbedtls_x509write_crt_set_version(crt, MBEDTLS_X509_CRT_VERSION_3);
    mbedtls_x509write_crt_set_md_alg(crt, MBEDTLS_MD_SHA256);
    mbedtls_x509write_crt_set_subject_key(crt, &g.key);
    mbedtls_x509write_crt_set_issuer_key(crt, &g.key);
    if (failed(mbedtls_x509write_crt_set_subject_name(crt, name.c_str()), "subject", error) ||
        failed(mbedtls_x509write_crt_set_issuer_name(crt, name.c_str()), "issuer", error) ||
        failed(mbedtls_x509write_crt_set_serial(crt, &g.serial), "serial", error) ||
        failed(mbedtls_x509write_crt_set_validity(crt, kTlsNotBefore, kTlsNotAfter), "validity", error) ||
        failed(mbedtls_x509write_crt_set_basic_constraints(crt, 0, -1), "basic constraints", error) ||
        failed(mbedtls_x509write_crt_set_key_usage(crt, MBEDTLS_X509_KU_DIGITAL_SIGNATURE), "key usage", error) ||
        failed(mbedtls_x509write_crt_set_extension(crt, MBEDTLS_OID_EXTENDED_KEY_USAGE,
                                                   MBEDTLS_OID_SIZE(MBEDTLS_OID_EXTENDED_KEY_USAGE), 0, kServerAuth,
                                                   sizeof(kServerAuth)),
               "extended key usage", error) ||
        failed(mbedtls_x509write_crt_set_extension(crt, MBEDTLS_OID_SUBJECT_ALT_NAME,
                                                   MBEDTLS_OID_SIZE(MBEDTLS_OID_SUBJECT_ALT_NAME), 0, altNames.data(),
                                                   altNames.size()),
               "subject alt name", error) ||
        failed(mbedtls_x509write_crt_set_subject_key_identifier(crt), "subject key id", error) ||
        failed(mbedtls_x509write_crt_set_authority_key_identifier(crt), "authority key id", error))
        return false;

    std::vector<unsigned char> pem(2048);
    if (failed(mbedtls_x509write_crt_pem(crt, pem.data(), pem.size(), mbedtls_ctr_drbg_random, &g.drbg),
               "certificate", error))
        return false;
    identity.certificatePem = reinterpret_cast<const char*>(pem.data());
    std::fill(pem.begin(), pem.end(), 0);
    if (failed(mbedtls_pk_write_key_pem(&g.key, pem.data(), pem.size()), "key export", error)) return false;
    identity.privateKeyPem = reinterpret_cast<const char*>(pem.data());
    std::fill(pem.begin(), pem.end(), 0);
    return true;
}

bool validTlsIdentity(const TlsIdentity& identity) {
    if (identity.certificatePem.empty() || identity.privateKeyPem.empty()) return false;
    mbedtls_x509_crt certificate;
    mbedtls_pk_context key;
    mbedtls_x509_crt_init(&certificate);
    mbedtls_pk_init(&key);
    const bool valid =
        mbedtls_x509_crt_parse(&certificate, reinterpret_cast<const unsigned char*>(identity.certificatePem.c_str()),
                               identity.certificatePem.size() + 1) == 0 &&
        mbedtls_pk_parse_key(&key, reinterpret_cast<const unsigned char*>(identity.privateKeyPem.c_str()),
                             identity.privateKeyPem.size() + 1, nullptr, 0) == 0 &&
        mbedtls_pk_check_pair(&certificate.pk, &key) == 0 && certificate.valid_to.year == 9999;
    mbedtls_pk_free(&key);
    mbedtls_x509_crt_free(&certificate);
    return valid;
}

#ifdef ARDUINO
bool loadOrCreateTlsIdentity(const std::string& dnsName, const std::array<std::uint8_t, 4>& ipAddress,
                             TlsIdentity& identity) {
    Preferences prefs;
    if (prefs.begin("sftls", true)) {
        identity.certificatePem = prefs.getString("cert", "").c_str();
        identity.privateKeyPem = prefs.getString("key", "").c_str();
        prefs.end();
        if (validTlsIdentity(identity)) return true;
    }

    Serial.println("[Web] generating self-signed TLS certificate (valid until 9999-12-31)");
    std::string error;
    if (!generateTlsIdentity(dnsName, ipAddress, identity, error) || !validTlsIdentity(identity)) {
        Serial.printf("[Web] TLS certificate generation failed: %s\n", error.empty() ? "invalid result" : error.c_str());
        identity = TlsIdentity{};
        return false;
    }
    if (!prefs.begin("sftls", false) || prefs.putString("cert", identity.certificatePem.c_str()) == 0 ||
        prefs.putString("key", identity.privateKeyPem.c_str()) == 0)
        Serial.println("[Web] storing the TLS certificate failed; a new one is generated on the next boot");
    prefs.end();
    return true;
}
#endif

}  // namespace web
