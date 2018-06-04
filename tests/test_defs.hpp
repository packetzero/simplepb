#ifndef _TEST_DEFS_HPP_
#define _TEST_DEFS_HPP_


#define ENC_GTEST_LOG_ENABLED 0

enum MY_FIELDS {
  MY_FIELD_A = 2,
  MY_FIELD_B = 54,
  MY_FIELD_C = 102
};

#ifndef MIN
#define MIN(a,b) (a)<(b)?(a):(b)
#endif

struct Person {
  int32_t     age;
  bool        active;
  char        name[3];
};

#define PERSON(DEST,NAME,AGE,ACTIVE) { \
  DEST = Person(); \
  strncpy(DEST.name,NAME,MIN(strlen(NAME),sizeof(DEST.name))); \
  DEST.age = AGE; \
  DEST.active = ACTIVE; \
}

void BytesToHexString(const unsigned char *bytes, size_t len, std::string &dest);
void BytesToHexString(const Bytes &vec, std::string &dest);
void BytesToHexString(const std::string &bytes, std::string &dest);


#endif // _TEST_DEFS_HPP_
