#include <Python.h>

#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <string.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <fcntl.h>
#include <unistd.h>

#if defined (__SVR4) && defined (__sun)
#include <sys/isa_defs.h>
#endif

#if (defined(BYTE_ORDER) && (BYTE_ORDER == BIG_ENDIAN)) ||  \
    (defined(__BYTE_ORDER__) && (__BYTE_ORDER__ == __ORDER_BIG_ENDIAN__)) || \
    (defined(_BIG_ENDIAN) && defined(__SVR4)&&defined(__sun))
#define BORG_BIG_ENDIAN 1
#elif (defined(BYTE_ORDER) && (BYTE_ORDER == LITTLE_ENDIAN)) || \
      (defined(__BYTE_ORDER__) && (__BYTE_ORDER__ == __ORDER_LITTLE_ENDIAN__)) || \
      (defined(_LITTLE_ENDIAN) && defined(__SVR4)&&defined(__sun))
#define BORG_BIG_ENDIAN 0
#else
#error Unknown byte order
#endif

#if BORG_BIG_ENDIAN
#define _le32toh(x) __builtin_bswap32(x)
#define _htole32(x) __builtin_bswap32(x)
#else
#define _le32toh(x) (x)
#define _htole32(x) (x)
#endif

#define MAGIC "BORG_IDX"
#define MAGIC_LEN 8

typedef struct {
    char magic[MAGIC_LEN];
    int32_t num_entries;
    int32_t num_buckets;
    int8_t  key_size;
    int8_t  value_size;
} __attribute__((__packed__)) HashHeader;

typedef struct {
    void *buckets;
    int num_entries;
    int num_buckets;
    int num_empty;
    int key_size;
    int value_size;
    off_t bucket_size;
    int lower_limit;
    int upper_limit;
    int min_empty;
    /* buckets may be backed by a Python buffer. If buckets_buffer.buf is NULL then this is not used. */
    Py_buffer buckets_buffer;
} HashIndex;

/* prime (or w/ big prime factors) hash table sizes
 * not sure we need primes for borg's usage (as we have a hash function based
 * on sha256, we can assume an even, seemingly random distribution of values),
 * but OTOH primes don't harm.
 * also, growth of the sizes starts with fast-growing 2x steps, but slows down
 * more and more down to 1.1x. this is to avoid huge jumps in memory allocation,
 * like e.g. 4G -> 8G.
 * these values are generated by hash_sizes.py.
 */
static int hash_sizes[] = {
    1031, 2053, 4099, 8209, 16411, 32771, 65537, 131101, 262147, 445649,
    757607, 1287917, 2189459, 3065243, 4291319, 6007867, 8410991,
    11775359, 16485527, 23079703, 27695653, 33234787, 39881729, 47858071,
    57429683, 68915617, 82698751, 99238507, 119086189, 144378011, 157223263,
    173476439, 190253911, 209915011, 230493629, 253169431, 278728861,
    306647623, 337318939, 370742809, 408229973, 449387209, 493428073,
    543105119, 596976533, 657794869, 722676499, 795815791, 874066969,
    962279771, 1057701643, 1164002657, 1280003147, 1407800297, 1548442699,
    1703765389, 1873768367, 2062383853, /* 32bit int ends about here */
};

#define HASH_MIN_LOAD .25
#define HASH_MAX_LOAD .75  /* don't go higher than 0.75, otherwise performance severely suffers! */
#define HASH_MAX_EFF_LOAD .93

#define MAX(x, y) ((x) > (y) ? (x): (y))
#define NELEMS(x) (sizeof(x) / sizeof((x)[0]))

#define EMPTY _htole32(0xffffffff)
#define DELETED _htole32(0xfffffffe)

#define BUCKET_ADDR(index, idx) (index->buckets + (idx * index->bucket_size))

#define BUCKET_MATCHES_KEY(index, idx, key) (memcmp(key, BUCKET_ADDR(index, idx), index->key_size) == 0)

#define BUCKET_IS_DELETED(index, idx) (*((uint32_t *)(BUCKET_ADDR(index, idx) + index->key_size)) == DELETED)
#define BUCKET_IS_EMPTY(index, idx) (*((uint32_t *)(BUCKET_ADDR(index, idx) + index->key_size)) == EMPTY)

#define BUCKET_MARK_DELETED(index, idx) (*((uint32_t *)(BUCKET_ADDR(index, idx) + index->key_size)) = DELETED)
#define BUCKET_MARK_EMPTY(index, idx) (*((uint32_t *)(BUCKET_ADDR(index, idx) + index->key_size)) = EMPTY)

#define EPRINTF_MSG(msg, ...) fprintf(stderr, "hashindex: " msg "\n", ##__VA_ARGS__)
#define EPRINTF_MSG_PATH(path, msg, ...) fprintf(stderr, "hashindex: %s: " msg "\n", path, ##__VA_ARGS__)
#define EPRINTF(msg, ...) fprintf(stderr, "hashindex: " msg "(%s)\n", ##__VA_ARGS__, strerror(errno))
#define EPRINTF_PATH(path, msg, ...) fprintf(stderr, "hashindex: %s: " msg " (%s)\n", path, ##__VA_ARGS__, strerror(errno))

static HashIndex *hashindex_read(PyObject *file_py);
static void hashindex_write(HashIndex *index, PyObject *file_py);
static HashIndex *hashindex_init(int capacity, int key_size, int value_size);
static const void *hashindex_get(HashIndex *index, const void *key);
static int hashindex_set(HashIndex *index, const void *key, const void *value);
static int hashindex_delete(HashIndex *index, const void *key);
static void *hashindex_next_key(HashIndex *index, const void *key);

/* Private API */
static void hashindex_free(HashIndex *index);

static void
hashindex_free_buckets(HashIndex *index)
{
    if(index->buckets_buffer.buf) {
        PyBuffer_Release(&index->buckets_buffer);
    } else {
        free(index->buckets);
    }
}

static int
hashindex_index(HashIndex *index, const void *key)
{
    return _le32toh(*((uint32_t *)key)) % index->num_buckets;
}

static int
hashindex_lookup(HashIndex *index, const void *key, int *start_idx)
{
    int didx = -1;
    int start = hashindex_index(index, key);
    int idx = start;
    for(;;) {
        if(BUCKET_IS_EMPTY(index, idx))
        {
            break;
        }
        if(BUCKET_IS_DELETED(index, idx)) {
            if(didx == -1) {
                didx = idx;
            }
        }
        else if(BUCKET_MATCHES_KEY(index, idx, key)) {
            if (didx != -1) {
                memcpy(BUCKET_ADDR(index, didx), BUCKET_ADDR(index, idx), index->bucket_size);
                BUCKET_MARK_DELETED(index, idx);
                idx = didx;
            }
            return idx;
        }
        idx = (idx + 1) % index->num_buckets;
        if(idx == start) {
            break;
        }
    }
    if (start_idx != NULL) {
        (*start_idx) = (didx == -1) ? idx : didx;
    }
    return -1;
}

static int
hashindex_resize(HashIndex *index, int capacity)
{
    HashIndex *new;
    void *key = NULL;
    int32_t key_size = index->key_size;

    if(!(new = hashindex_init(capacity, key_size, index->value_size))) {
        return 0;
    }
    while((key = hashindex_next_key(index, key))) {
        if(!hashindex_set(new, key, key + key_size)) {
            /* This can only happen if there's a bug in the code calculating capacity */
            hashindex_free(new);
            return 0;
        }
    }
    hashindex_free_buckets(index);
    index->buckets = new->buckets;
    index->num_buckets = new->num_buckets;
    index->num_empty = index->num_buckets - index->num_entries;
    index->lower_limit = new->lower_limit;
    index->upper_limit = new->upper_limit;
    index->min_empty = new->min_empty;
    free(new);
    return 1;
}

int get_lower_limit(int num_buckets){
    int min_buckets = hash_sizes[0];
    if (num_buckets <= min_buckets)
        return 0;
    return (int)(num_buckets * HASH_MIN_LOAD);
}

int get_upper_limit(int num_buckets){
    int max_buckets = hash_sizes[NELEMS(hash_sizes) - 1];
    if (num_buckets >= max_buckets)
        return num_buckets;
    return (int)(num_buckets * HASH_MAX_LOAD);
}

int get_min_empty(int num_buckets){
    /* Differently from load, the effective load also considers tombstones (deleted buckets). */
    return (int)(num_buckets * (1.0 - HASH_MAX_EFF_LOAD));
}

int size_idx(int size){
    /* find the hash_sizes index with entry >= size */
    int elems = NELEMS(hash_sizes);
    int entry, i=0;
    do{
        entry = hash_sizes[i++];
    }while((entry < size) && (i < elems));
    if (i >= elems)
        return elems - 1;
    i--;
    return i;
}

int fit_size(int current){
    int i = size_idx(current);
    return hash_sizes[i];
}

int grow_size(int current){
    int i = size_idx(current) + 1;
    int elems = NELEMS(hash_sizes);
    if (i >= elems)
        return hash_sizes[elems - 1];
    return hash_sizes[i];
}

int shrink_size(int current){
    int i = size_idx(current) - 1;
    if (i < 0)
        return hash_sizes[0];
    return hash_sizes[i];
}

int
count_empty(HashIndex *index)
{   /* count empty (never used) buckets. this does NOT include deleted buckets (tombstones).
     * TODO: if we ever change HashHeader, save the count there so we do not need this function.
     */
    int i, count = 0, capacity = index->num_buckets;
    for(i = 0; i < capacity; i++) {
        if(BUCKET_IS_EMPTY(index, i))
            count++;
    }
    return count;
}

/* Public API */

static HashIndex *
hashindex_read(PyObject *file_py)
{
    Py_ssize_t length, buckets_length, bytes_read;
    Py_buffer header_buffer;
    PyObject *header_bytes, *length_object, *bucket_bytes;
    HashHeader *header;
    HashIndex *index = NULL;

    header_bytes = PyObject_CallMethod(file_py, "read", "n", (Py_ssize_t)sizeof(HashHeader));
    if(!header_bytes) {
        assert(PyErr_Occurred());
        goto fail;
    }

    bytes_read = PyBytes_Size(header_bytes);
    if(PyErr_Occurred()) {
        /* TypeError, not a bytes() object */
        goto fail_decref_header;
    }
    if(bytes_read != sizeof(HashHeader)) {
        /* Truncated file */
        /* Note: %zd is the format for Py_ssize_t, %zu is for size_t */
        PyErr_Format(PyExc_ValueError, "Could not read header (expected %zu, but read %zd bytes)",
                     sizeof(HashHeader), bytes_read);
        goto fail_decref_header;
    }

    /*
     * Hash the header
     * If the header is corrupted this bails before doing something stupid (like allocating 3.8 TB of memory)
     */
    Py_XDECREF(PyObject_CallMethod(file_py, "hash_part", "s", "HashHeader"));
    if(PyErr_Occurred()) {
        if(PyErr_ExceptionMatches(PyExc_AttributeError)) {
            /* Be able to work with regular file objects which do not have a hash_part method. */
            PyErr_Clear();
        } else {
            goto fail_decref_header;
        }
    }

    /* Find length of file */
    length_object = PyObject_CallMethod(file_py, "seek", "ni", (Py_ssize_t)0, SEEK_END);
    if(PyErr_Occurred()) {
        goto fail_decref_header;
    }
    length = PyNumber_AsSsize_t(length_object, PyExc_OverflowError);
    Py_DECREF(length_object);
    if(PyErr_Occurred()) {
        /* This shouldn't generally happen; but can if seek() returns something that's not a number */
        goto fail_decref_header;
    }

    Py_XDECREF(PyObject_CallMethod(file_py, "seek", "ni", (Py_ssize_t)sizeof(HashHeader), SEEK_SET));
    if(PyErr_Occurred()) {
        goto fail_decref_header;
    }

    /* Set up the in-memory header */
    if(!(index = malloc(sizeof(HashIndex)))) {
        PyErr_NoMemory();
        goto fail_decref_header;
    }

    PyObject_GetBuffer(header_bytes, &header_buffer, PyBUF_SIMPLE);
    if(PyErr_Occurred()) {
        goto fail_free_index;
    }

    header = (HashHeader*) header_buffer.buf;
    if(memcmp(header->magic, MAGIC, MAGIC_LEN)) {
        PyErr_Format(PyExc_ValueError, "Unknown MAGIC in header");
        goto fail_release_header_buffer;
    }

    buckets_length = (Py_ssize_t)_le32toh(header->num_buckets) * (header->key_size + header->value_size);
    if((Py_ssize_t)length != (Py_ssize_t)sizeof(HashHeader) + buckets_length) {
        PyErr_Format(PyExc_ValueError, "Incorrect file length (expected %zd, got %zd)",
                     sizeof(HashHeader) + buckets_length, length);
        goto fail_release_header_buffer;
    }

    index->num_entries = _le32toh(header->num_entries);
    index->num_buckets = _le32toh(header->num_buckets);
    index->key_size = header->key_size;
    index->value_size = header->value_size;
    index->bucket_size = index->key_size + index->value_size;
    index->lower_limit = get_lower_limit(index->num_buckets);
    index->upper_limit = get_upper_limit(index->num_buckets);

    /*
     * For indices read from disk we don't malloc() the buckets ourselves,
     * we have them backed by a Python bytes() object instead, and go through
     * Python I/O.
     *
     * Note: Issuing read(buckets_length) is okay here, because buffered readers
     * will issue multiple underlying reads if necessary. This supports indices
     * >2 GB on Linux. We also compare lengths later.
     */
    bucket_bytes = PyObject_CallMethod(file_py, "read", "n", buckets_length);
    if(!bucket_bytes) {
        assert(PyErr_Occurred());
        goto fail_release_header_buffer;
    }
    bytes_read = PyBytes_Size(bucket_bytes);
    if(PyErr_Occurred()) {
        /* TypeError, not a bytes() object */
        goto fail_decref_buckets;
    }
    if(bytes_read != buckets_length) {
        PyErr_Format(PyExc_ValueError, "Could not read buckets (expected %zd, got %zd)", buckets_length, bytes_read);
        goto fail_decref_buckets;
    }

    PyObject_GetBuffer(bucket_bytes, &index->buckets_buffer, PyBUF_SIMPLE);
    if(PyErr_Occurred()) {
        goto fail_decref_buckets;
    }
    index->buckets = index->buckets_buffer.buf;

    index->min_empty = get_min_empty(index->num_buckets);
    index->num_empty = count_empty(index);

    if(index->num_empty < index->min_empty) {
        /* too many tombstones here / not enough empty buckets, do a same-size rebuild */
        if(!hashindex_resize(index, index->num_buckets)) {
            PyErr_Format(PyExc_ValueError, "Failed to rebuild table");
            goto fail_free_buckets;
        }
    }

    /*
     * Clean intermediary objects up. Note that index is only freed if an error has occurred.
     * Also note that the buffer in index->buckets_buffer holds a reference to buckets_bytes.
     */

fail_free_buckets:
    if(PyErr_Occurred()) {
        hashindex_free_buckets(index);
    }
fail_decref_buckets:
    Py_DECREF(bucket_bytes);
fail_release_header_buffer:
    PyBuffer_Release(&header_buffer);
fail_free_index:
    if(PyErr_Occurred()) {
        free(index);
        index = NULL;
    }
fail_decref_header:
    Py_DECREF(header_bytes);
fail:
    return index;
}

static HashIndex *
hashindex_init(int capacity, int key_size, int value_size)
{
    HashIndex *index;
    int i;
    capacity = fit_size(capacity);

    if(!(index = malloc(sizeof(HashIndex)))) {
        EPRINTF("malloc header failed");
        return NULL;
    }
    if(!(index->buckets = calloc(capacity, key_size + value_size))) {
        EPRINTF("malloc buckets failed");
        free(index);
        return NULL;
    }
    index->num_entries = 0;
    index->key_size = key_size;
    index->value_size = value_size;
    index->num_buckets = capacity;
    index->num_empty = capacity;
    index->bucket_size = index->key_size + index->value_size;
    index->lower_limit = get_lower_limit(index->num_buckets);
    index->upper_limit = get_upper_limit(index->num_buckets);
    index->min_empty = get_min_empty(index->num_buckets);
    index->buckets_buffer.buf = NULL;
    for(i = 0; i < capacity; i++) {
        BUCKET_MARK_EMPTY(index, i);
    }
    return index;
}

static void
hashindex_free(HashIndex *index)
{
    hashindex_free_buckets(index);
    free(index);
}


static void
hashindex_write(HashIndex *index, PyObject *file_py)
{
    PyObject *length_object, *buckets_view;
    Py_ssize_t length;
    Py_ssize_t buckets_length = (Py_ssize_t)index->num_buckets * index->bucket_size;
    HashHeader header = {
        .magic = MAGIC,
        .num_entries = _htole32(index->num_entries),
        .num_buckets = _htole32(index->num_buckets),
        .key_size = index->key_size,
        .value_size = index->value_size
    };

    length_object = PyObject_CallMethod(file_py, "write", "y#", &header, (int)sizeof(HashHeader));
    if(PyErr_Occurred()) {
        return;
    }
    length = PyNumber_AsSsize_t(length_object, PyExc_OverflowError);
    Py_DECREF(length_object);
    if(PyErr_Occurred()) {
        return;
    }
    if(length != sizeof(HashHeader)) {
        PyErr_SetString(PyExc_ValueError, "Failed to write header");
        return;
    }

    /*
     * Hash the header
     */
    Py_XDECREF(PyObject_CallMethod(file_py, "hash_part", "s", "HashHeader"));
    if(PyErr_Occurred()) {
        if(PyErr_ExceptionMatches(PyExc_AttributeError)) {
            /* Be able to work with regular file objects which do not have a hash_part method. */
            PyErr_Clear();
        } else {
            return;
        }
    }

    /* Note: explicitly construct view; BuildValue can convert (pointer, length) to Python objects, but copies them for doing so */
    buckets_view = PyMemoryView_FromMemory((char*)index->buckets, buckets_length, PyBUF_READ);
    if(!buckets_view) {
        assert(PyErr_Occurred());
        return;
    }
    length_object = PyObject_CallMethod(file_py, "write", "O", buckets_view);
    Py_DECREF(buckets_view);
    if(PyErr_Occurred()) {
        return;
    }
    length = PyNumber_AsSsize_t(length_object, PyExc_OverflowError);
    Py_DECREF(length_object);
    if(PyErr_Occurred()) {
        return;
    }
    if(length != buckets_length) {
        PyErr_SetString(PyExc_ValueError, "Failed to write buckets");
        return;
    }
}

static const void *
hashindex_get(HashIndex *index, const void *key)
{
    int idx = hashindex_lookup(index, key, NULL);
    if(idx < 0) {
        return NULL;
    }
    return BUCKET_ADDR(index, idx) + index->key_size;
}

static int
hashindex_set(HashIndex *index, const void *key, const void *value)
{
    int start_idx;
    int idx = hashindex_lookup(index, key, &start_idx);
    uint8_t *ptr;
    if(idx < 0)
    {
        if(index->num_entries > index->upper_limit) {
            if(!hashindex_resize(index, grow_size(index->num_buckets))) {
                return 0;
            }
            start_idx = hashindex_index(index, key);
        }
        idx = start_idx;
        while(!BUCKET_IS_EMPTY(index, idx) && !BUCKET_IS_DELETED(index, idx)) {
            idx = (idx + 1) % index->num_buckets;
        }
        if(BUCKET_IS_EMPTY(index, idx)){
            index->num_empty--;
            if(index->num_empty < index->min_empty) {
                /* too many tombstones here / not enough empty buckets, do a same-size rebuild */
                if(!hashindex_resize(index, index->num_buckets)) {
                    return 0;
                }
            }
        }
        ptr = BUCKET_ADDR(index, idx);
        memcpy(ptr, key, index->key_size);
        memcpy(ptr + index->key_size, value, index->value_size);
        index->num_entries += 1;
    }
    else
    {
        memcpy(BUCKET_ADDR(index, idx) + index->key_size, value, index->value_size);
    }
    return 1;
}

static int
hashindex_delete(HashIndex *index, const void *key)
{
    int idx = hashindex_lookup(index, key, NULL);
    if (idx < 0) {
        return 1;
    }
    BUCKET_MARK_DELETED(index, idx);
    index->num_entries -= 1;
    if(index->num_entries < index->lower_limit) {
        if(!hashindex_resize(index, shrink_size(index->num_buckets))) {
            return 0;
        }
    }
    return 1;
}

static void *
hashindex_next_key(HashIndex *index, const void *key)
{
    int idx = 0;
    if(key) {
        idx = 1 + (key - index->buckets) / index->bucket_size;
    }
    if (idx == index->num_buckets) {
        return NULL;
    }
    while(BUCKET_IS_EMPTY(index, idx) || BUCKET_IS_DELETED(index, idx)) {
        idx ++;
        if (idx == index->num_buckets) {
            return NULL;
        }
    }
    return BUCKET_ADDR(index, idx);
}

static int
hashindex_len(HashIndex *index)
{
    return index->num_entries;
}

static int
hashindex_size(HashIndex *index)
{
    return sizeof(HashHeader) + index->num_buckets * index->bucket_size;
}
