/**
 * PANDA 3D SOFTWARE
 * Copyright (c) Carnegie Mellon University.  All rights reserved.
 *
 * All use of this software is subject to the terms of the revised BSD
 * license.  You should have received a copy of this license along
 * with this source code in a file named "LICENSE."
 *
 * @file makePrcKey.cxx
 * @author drose
 * @date 2004-10-19
 */

#include "dtoolbase.h"
#include "prcKeyRegistry.h"
#include "filename.h"
#include "pvector.h"
#include "panda_getopt.h"
#include "preprocess_argv.h"
#include <stdio.h>

// Pick up the public key definitions.
#ifdef PRC_PUBLIC_KEYS_INCLUDE
#include PRC_PUBLIC_KEYS_INCLUDE
#endif

#include <openssl/rsa.h>
#include <openssl/err.h>
#include <openssl/pem.h>
#include <openssl/rand.h>
#include <openssl/bio.h>

using std::cerr;
using std::string;

class KeyNumber {
public:
  int _number;
  bool _got_pass_phrase;
  string _pass_phrase;
};
typedef pvector<KeyNumber> KeyNumbers;

/**
 * A convenience function that is itself a wrapper around the OpenSSL
 * convenience function to output the recent OpenSSL errors.  This function
 * sends the error string to cerr.
 */
void
output_ssl_errors() {
  cerr << "Error occurred in SSL routines.\n";

  static bool strings_loaded = false;
  if (!strings_loaded) {
    ERR_load_crypto_strings();
    strings_loaded = true;
  }

  unsigned long e = ERR_get_error();
  while (e != 0) {
    static const size_t buffer_len = 256;
    char buffer[buffer_len];
    ERR_error_string_n(e, buffer, buffer_len);
    cerr << buffer << "\n";
    e = ERR_get_error();
  }
}

/**
 * Extracts the data written to the indicated memory bio and writes it to the
 * indicated stream, formatting it to be compiled into a C or C++ program as a
 * string.
 */
void
output_c_string(std::ostream &out, const string &string_name,
                size_t index, BIO *mbio) {
  char *data_ptr;
  size_t data_size = BIO_get_mem_data(mbio, &data_ptr);

  out << "static const char * const " << string_name
      << index << "_data =\n"
      << "  \"";

  bool last_nl = false;
  for (size_t i = 0; i < data_size; i++) {
    if (data_ptr[i] == '\n') {
      out << "\\n";
      last_nl = true;

    } else {
      if (last_nl) {
        out << "\"\n  \"";
        last_nl = false;
      }

      if (data_ptr[i] == '\t') {
        out << "\\t";
      }
      else if (isprint(data_ptr[i])) {
        out << data_ptr[i];
      }
      else {
        out << "\\x" << std::hex << std::setw(2) << std::setfill('0')
            << (unsigned int)(unsigned char)data_ptr[i] << std::dec;
      }
    }
  }
  out << "\";\nstatic const unsigned int " << string_name << index
      << "_length = " << data_size << ";\n";
}

/**
 * Generates a new public and private key pair.
 */
EVP_PKEY *
generate_key() {
  RSA *rsa = RSA_new();
  BIGNUM *e = BN_new();
  if (rsa == nullptr || e == nullptr) {
    output_ssl_errors();
    exit(1);
  }

  BN_set_word(e, 7);

  if (!RSA_generate_key_ex(rsa, 1024, e, nullptr)) {
    BN_free(e);
    RSA_free(rsa);
    output_ssl_errors();
    exit(1);
  }
  BN_free(e);

  EVP_PKEY *pkey = EVP_PKEY_new();
  EVP_PKEY_assign_RSA(pkey, rsa);
  return pkey;
}

/**
 * Writes the list of public keys stored in the PrcKeyRegistry to the
 * indicated output filename as a compilable list of KeyDef entries, suitable
 * for passing to PrcKeyRegistry::record_keys().
 */
void
write_public_keys(Filename outfile) {
  outfile.set_text();
  cerr << "Rewriting " << outfile << "\n";

  pofstream out;
  if (!outfile.open_write(out)) {
    cerr << "Unable to open " << outfile << " for writing.\n";
    exit(1);
  }

  out <<
    "\n"
    "// This file was generated by make-prc-key.  It defines the public keys\n"
    "// that will be used to validate signed prc files.\n"
    "\n"
    "#include \"prcKeyRegistry.h\"\n"
    "\n";

  PrcKeyRegistry *pkr = PrcKeyRegistry::get_global_ptr();

  BIO *mbio = BIO_new(BIO_s_mem());

  size_t num_keys = pkr->get_num_keys();
  size_t i;
  for (i = 0; i < num_keys; i++) {
    EVP_PKEY *pkey = pkr->get_key(i);

    if (pkey != nullptr) {
      if (!PEM_write_bio_PUBKEY(mbio, pkey)) {
        output_ssl_errors();
        exit(1);
      }

      output_c_string(out, "prc_pubkey", i, mbio);
      (void)BIO_reset(mbio);
      out << "\n";
    }
  }

  BIO_free(mbio);

  // Now output the table that indexes all of the above.
  out << "static PrcKeyRegistry::KeyDef const prc_pubkeys[" << num_keys << "] = {\n";

  for (i = 0; i < num_keys; i++) {
    EVP_PKEY *pkey = pkr->get_key(i);
    time_t generated_time = pkr->get_generated_time(i);

    if (pkey != nullptr) {
      out << "  { prc_pubkey" << i << "_data, prc_pubkey" << i
          << "_length, " << generated_time << " },\n";
    } else {
      out << "  { nullptr, 0, 0 },\n";
    }
  };

  out << "};\n"
      << "static const int num_prc_pubkeys = " << num_keys << ";\n\n";
}

/**
 * Generates a C++ program that can be used to sign a prc file with the
 * indicated private key into the given output filename.
 */
void
write_private_key(EVP_PKEY *pkey, Filename outfile, int n, time_t now,
                  const char *pp) {
  outfile.set_text();
  cerr << "Rewriting " << outfile << "\n";

  pofstream out;
  if (!outfile.open_write(out)) {
    cerr << "Unable to open " << outfile << " for writing.\n";
    exit(1);
  }

  out <<
    "\n"
    "// This file was generated by make-prc-key.  It can be compiled against\n"
    "// dtool to produce a program that will sign a prc file using key number " << n << ".\n\n";

  BIO *mbio = BIO_new(BIO_s_mem());

  int write_result;
  if (pp != nullptr && *pp == '\0') {
    // The supplied password was the empty string.  This means not to encrypt
    // the private key.
    write_result =
      PEM_write_bio_PKCS8PrivateKey(mbio, pkey, nullptr, nullptr, 0, nullptr, nullptr);

  } else {
    // Otherwise, the default is to encrypt it.
    write_result =
      PEM_write_bio_PKCS8PrivateKey(mbio, pkey, EVP_des_ede3_cbc(),
                                    nullptr, 0, nullptr, (void *)pp);
  }

  if (!write_result) {
    output_ssl_errors();
    exit(1);
  }

  output_c_string(out, "prc_privkey", n, mbio);

  BIO_free(mbio);

  out <<
    "\n\n"
    "#define KEY_NUMBER " << n << "\n"
    "#define KEY_DATA prc_privkey" << n << "_data\n"
    "#define KEY_LENGTH prc_privkey" << n << "_length\n"
    "#define PROGNAME \"" << outfile.get_basename_wo_extension() << "\"\n"
    "#define GENERATED_TIME " << now << "\n\n"

    "#include \"signPrcFile_src.cxx\"\n\n";
}

/**
 *
 */
void
usage() {
  cerr <<
    "\nmake-prc-key [opts] 1[,\"pass_phrase\"] [2[,\"pass phrase\"] 3 ...]\n\n"

    "This program generates one or more new keys to be used for signing\n"
    "a prc file.  The key itself is a completely arbitrary random bit\n"
    "sequence.  It is divided into a public and a private key; the public\n"
    "key is not secret and will be compiled into libdtool, while the private\n"
    "key should be safeguarded and will be written into a .cxx file that\n"
    "can be compiled as a standalone application.\n\n"

    "The output is a public and private key pair for each trust level.  The\n"
    "form of the output for both public and private keys will be compilable\n"
    "C++ code; see -a and -b, below, for a complete description.\n\n"

    "After the options, the remaining arguments list the individual trust\n"
    "level keys to generate.  For each integer specified, a different key\n"
    "will be created.  There should be one key for each trust level\n"
    "required; a typical application will only need one or two keys.\n\n"

    "Options:\n\n"

    "   -a pub_outfile.cxx\n"
    "       Specifies the name and location of the public key output file\n"
    "       to generate.  This file must then be named by the Config.pp\n"
    "       variable PRC_PUBLIC_KEYS_FILENAME so that it will be compiled\n"
    "       in with libdtool and available to verify signatures.  If this\n"
    "       option is omitted, the previously-compiled value is used.\n\n"

    "   -b priv_outfile#.cxx\n"
    "       Specifies the name and location of the private key output file(s)\n"
    "       to generate.  A different output file will be generated for each\n"
    "       different trust level; the hash mark '#' appearing in the file\n"
    "       name will be filled in with the corresponding numeric trust level.\n"
    "       The hash mark may be omitted if you only require one trust level.\n"
    "       When compiled against dtool, each of these files will generate\n"
    "       a program that can be used to sign a prc file with the corresponding\n"
    "       trust level.\n\n"

    "   -p \"[pass phrase]\"\n"
    "       Uses the indicated pass phrase to encrypt the private key.\n"
    "       This specifies an overall pass phrase; you may also specify\n"
    "       a different pass phrase for each key by using the key,\"pass phrase\"\n"
    "       syntax.\n\n"

    "       If a pass phrase is not specified on the command line, you will be\n"
    "       prompted interactively.  Every user of the signing programs\n"
    "       (outfile_sign1.cxx, etc.) will need to know the pass phrase\n"
    "       in order to sign prc files.\n\n"

    "       If this is specified as the empty string (\"\"), then the key\n"
    "       will not be encrypted, and anyone can run the signing\n"
    "       programs without having to supply a pass phrase.\n\n";
}

/**
 *
 */
int
main(int argc, char **argv) {
  extern char *optarg;
  extern int optind;
  const char *optstr = "a:b:p:h";

  Filename pub_outfile;
  bool got_pub_outfile = false;
  Filename priv_outfile;
  bool got_priv_outfile = false;
  string pass_phrase;
  bool got_pass_phrase = false;

  preprocess_argv(argc, argv);
  int flag = getopt(argc, argv, optstr);

  while (flag != EOF) {
    switch (flag) {
    case 'a':
      pub_outfile = optarg;
      got_pub_outfile = true;
      break;

    case 'b':
      priv_outfile = optarg;
      got_priv_outfile = true;
      break;

    case 'p':
      pass_phrase = optarg;
      got_pass_phrase = true;
      break;

    case 'h':
      usage();
      exit(0);

    default:
      exit(1);
    }
    flag = getopt(argc, argv, optstr);
  }

  argc -= (optind-1);
  argv += (optind-1);

  if (argc < 2) {
    usage();
    exit(1);
  }

  if (got_pub_outfile) {
    if (pub_outfile.get_extension() != "cxx") {
      cerr << "Public key output file '" << pub_outfile
           << "' should have a .cxx extension.\n";
      exit(1);
    }
  } else {
#ifdef PRC_PUBLIC_KEYS_INCLUDE
    PrcKeyRegistry::get_global_ptr()->record_keys(prc_pubkeys, num_prc_pubkeys);
    pub_outfile = PRC_PUBLIC_KEYS_FILENAME;
#endif

    if (pub_outfile.empty()) {
      cerr << "No -a specified, and no PRC_PUBLIC_KEYS_FILENAME variable\n"
           << "compiled in.\n\n";
      exit(1);
    }
  }

  if (got_priv_outfile) {
    if (priv_outfile.get_extension() != "cxx") {
      cerr << "Private key output file '" << priv_outfile
           << "' should have a .cxx extension.\n";
      exit(1);
    }

  } else {
    cerr << "You must use the -b option to specify the private key output filenames.\n";
    exit(1);
  }

  KeyNumbers key_numbers;
  for (int i = 1; i < argc; i++) {
    KeyNumber key;
    char *endptr;
    key._number = (int)strtol(argv[i], &endptr, 0);
    key._got_pass_phrase = got_pass_phrase;
    key._pass_phrase = pass_phrase;

    if (*endptr == ',') {
      // Here's a pass phrase for this particular key.
      key._got_pass_phrase = true;
      key._pass_phrase = endptr + 1;
    } else if (*endptr) {
      cerr << "Parameter '" << argv[i] << "' should be an integer.\n";
      exit(1);
    }
    if (key._number <= 0) {
      cerr << "Key numbers must be greater than 0; you specified "
           << key._number << ".\n";
      exit(1);
    }
    key_numbers.push_back(key);
  }

  // Seed the random number generator.
  RAND_status();

  // Load the OpenSSL algorithms.
  OpenSSL_add_all_algorithms();

  time_t now = time(nullptr);

  string name = priv_outfile.get_fullpath_wo_extension();
  string prefix, suffix;
  bool got_hash;

  size_t hash = name.find('#');
  if (hash == string::npos) {
    prefix = name;
    suffix = ".cxx";
    got_hash = false;

  } else {
    prefix = name.substr(0, hash);
    suffix = name.substr(hash + 1) + ".cxx";
    got_hash = true;
  }

  KeyNumbers::iterator ki;
  for (ki = key_numbers.begin(); ki != key_numbers.end(); ++ki) {
    int n = (*ki)._number;
    const char *pp = nullptr;
    if ((*ki)._got_pass_phrase) {
      pp = (*ki)._pass_phrase.c_str();
    }

    EVP_PKEY *pkey = generate_key();
    PrcKeyRegistry::get_global_ptr()->set_key(n, pkey, now);

    std::ostringstream strm;
    if (got_hash || n != 1) {
      // If we got an explicit hash mark, we always output the number.  If we
      // did not get an explicit hash mark, we output the number only if it is
      // other than 1.
      strm << prefix << n << suffix;

    } else {
      // If we did not get an explicit hash mark in the filename, we omit the
      // number for key 1 (this might be the only key, and so maybe the user
      // doesn't require a number designator).
      strm << prefix << suffix;
    }

    write_private_key(pkey, strm.str(), n, now, pp);
  }

  write_public_keys(pub_outfile);

  return (0);
}
