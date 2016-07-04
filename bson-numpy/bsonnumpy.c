#include <Python.h>
#include <stdlib.h>
#include <stdio.h>
#include <string.h>
#define NPY_NO_DEPRECATED_API NPY_1_7_API_VERSION
//#include <numpy/arrayobject.h>
//#include <numpy/npy_common.h>
#include <numpy/ndarrayobject.h>

#include "bson.h"

static PyObject* BsonNumpyError;

static PyObject*
ndarray_to_bson(PyObject* self, PyObject* args)
{
    PyObject* dtype_obj;
    PyObject* array_obj;
    PyArray_Descr* dtype;
    PyArrayObject* array;
    if (!PyArg_ParseTuple(args, "OO", &dtype_obj, &array_obj)) {
        PyErr_SetNone(PyExc_TypeError);
        return NULL;
    }
    // Convert dtype
    if (!PyArray_DescrCheck(dtype_obj)) {
        PyErr_SetNone(PyExc_TypeError);
        return NULL;
    }
    if (!PyArray_DescrConverter(dtype_obj, &dtype)) {
        PyErr_SetString(BsonNumpyError, "dtype passed in was invalid");
        return NULL;
    }

    // Convert array
    if (!PyArray_Check(array_obj)) {
        PyErr_SetNone(PyExc_TypeError);
        return NULL;
    }
    if (!PyArray_OutputConverter(array_obj, &array)) {
        PyErr_SetString(BsonNumpyError, "bad array type");
        return NULL;
    }
    return Py_BuildValue("");
}
/*
    |Straightforward
    case BSON_TYPE_INT64
    case BSON_TYPE_INT32
    case BSON_TYPE_DOUBLE
    case BSON_TYPE_BOOL

    case BSON_TYPE_OID
    case BSON_TYPE_UTF8
    case BSON_TYPE_BINARY
    case BSON_TYPE_SYMBOL
    case BSON_TYPE_CODE
    case BSON_TYPE_DATE_TIME

    case BSON_TYPE_DOCUMENT

    |Totally different case
    case BSON_TYPE_ARRAY:

    |With issues to work out:
    case BSON_TYPE_TIMESTAMP
    case BSON_TYPE_REGEX

    |Not clear what to do, maybe make a flexible type?
    case BSON_TYPE_DBPOINTER
    case BSON_TYPE_CODEWSCOPE

    Probably error, no bson_iter_ for
    case BSON_TYPE_UNDEFINED
    case BSON_TYPE_NULL
    case BSON_TYPE_MAXKEY
    case BSON_TYPE_MINKEY
    case BSON_TYPE_EOD
 */



static int _load_scalar(bson_iter_t* bsonit,
                       npy_intp* coordinates,
                       PyArrayObject* ndarray,
                       npy_intp depth,
                       npy_intp number_dimensions) {

        bson_iter_t sub_it;
        int itemsize = PyArray_DESCR(ndarray)->elsize;;
        int len = itemsize;
        int success = 0;
        int copy = 1;

        if(BSON_ITER_HOLDS_ARRAY(bsonit)) {
            bson_iter_recurse(bsonit, &sub_it);

            int i = 0;
            while( bson_iter_next(&sub_it) ) { // TODO: loop on ndarray not on bson, goign to have to pass dimensions from tuple
                coordinates[depth + 1] = i;
                _load_scalar(&sub_it, coordinates, ndarray, depth+1, number_dimensions);
                i++;
            }
            return 1; // TODO: check result of _load_scalar
        }
        void* pointer = PyArray_GetPtr(ndarray, coordinates);
        const bson_value_t* value = bson_iter_value(bsonit);
        void* data_ptr = (void*)&value->value;
        switch(value->value_type) {
        case BSON_TYPE_UTF8:
            data_ptr = value->value.v_utf8.str; // Unclear why using value->value doesn't work
            len = value->value.v_utf8.len;
            break;
        case BSON_TYPE_BINARY:
            data_ptr = value->value.v_binary.data;
            len = value->value.v_binary.data_len;
            break;
        case BSON_TYPE_SYMBOL: // deprecated
            data_ptr = value->value.v_symbol.symbol;
            len = value->value.v_symbol.len;
            break;
        case BSON_TYPE_CODE:
            data_ptr = value->value.v_code.code;
            len = value->value.v_code.code_len;
            break;
        case BSON_TYPE_DOCUMENT:
            // TODO: what about V lengths that are longer than the doc?
            data_ptr = value->value.v_doc.data;
            len = value->value.v_doc.data_len;
            break;

        // Have to special case for timestamp and regex bc there's no np equiv
        case BSON_TYPE_TIMESTAMP:
            memcpy(pointer, &value->value.v_timestamp.timestamp, sizeof(int32_t));
            memcpy((pointer+sizeof(int32_t)), &value->value.v_timestamp.increment, sizeof(int32_t));
            copy = 0;
            break;
        case BSON_TYPE_REGEX:
            len = (int)strlen(value->value.v_regex.regex);
            memcpy(pointer, value->value.v_regex.regex, len);
            memset(pointer + len, '\0', 1);
            memcpy(pointer + len + 1, value->value.v_regex.options, (int)strlen(value->value.v_regex.options));
            len = len + (int)strlen(value->value.v_regex.options) + 1;
            copy = 0;
            break;

        }

        if(copy && len == itemsize) {
            PyObject* data = PyArray_Scalar(data_ptr, PyArray_DESCR(ndarray), NULL);
//            printf("ITEM=");
//            PyObject_Print(data, stdout, 0);
//            printf("\n");
            success = PyArray_SETITEM(ndarray, pointer, data);
    //        Py_INCREF(data);
        }
        else if(copy) {
            // Dealing with data that's shorter than the array datatype, so we can't read using the macros.
            if(len > itemsize) {
                len = itemsize; // truncate data that's too big
            }
            memcpy(pointer, data_ptr, len);
            memset(pointer + len, '\0', itemsize - len);

        }
        return success;
}

static PyObject*
bson_to_ndarray(PyObject* self, PyObject* args)
{
    // Takes in a BSON byte string
    PyObject* binary_obj;
    PyObject* dtype_obj;
    PyObject *array_obj;
    const char* bytestr;
    PyArray_Descr* dtype;
    PyArrayObject* ndarray;
    Py_ssize_t bytes_len;
    Py_ssize_t number_dimensions = -1;
    npy_intp* dimension_lengths;
    bson_iter_t bsonit;
    bson_t* document;
    size_t err_offset;

    if (!PyArg_ParseTuple(args, "SO", &binary_obj, &dtype_obj)) {
        PyErr_SetNone(PyExc_TypeError);
        return NULL;
    }
    bytestr = PyBytes_AS_STRING(binary_obj);
    bytes_len = PyBytes_GET_SIZE(binary_obj);
    document = bson_new_from_data((uint8_t*)bytestr, bytes_len); // slower than what??? Also, is this a valid cast? TODO: free
    if (!bson_validate(document, BSON_VALIDATE_NONE, &err_offset)) {
     // TODO: validate in a reasonable way, now segfaults if bad
        PyErr_SetString(BsonNumpyError, "Document failed validation");
        return NULL;
    }
//    char* str = bson_as_json(document, (size_t*)&bytes_len);
//    printf("DOCUMENT: %s\n", str);

    // Convert dtype
    if (!PyArray_DescrCheck(dtype_obj)) {
        PyErr_SetNone(PyExc_TypeError);
        return NULL;
    }
    if (!PyArray_DescrConverter(dtype_obj, &dtype)) {
        PyErr_SetString(BsonNumpyError, "dtype passed in was invalid");
        return NULL;
    }

    bson_iter_init(&bsonit, document);
    dimension_lengths = malloc(1* sizeof(npy_intp));
    dimension_lengths[0] = bson_count_keys(document);
    number_dimensions = 1;

    if(dtype->subarray != NULL) {
        PyObject *shape = dtype->subarray->shape;
        if(!PyTuple_Check(shape)) {
            PyErr_SetString(BsonNumpyError, "dtype passed in was invalid");
            return NULL;
        }
        number_dimensions = (int)PyTuple_Size(shape);
    }

//    printf("dtype_obj=");
//    PyObject_Print(dtype_obj, stdout, 0);
//    printf("\n");

    Py_INCREF(dtype);

    array_obj = PyArray_Zeros(1, dimension_lengths, dtype, 0); // This function steals a reference to dtype?
//    printf("array_obj=");
//    PyObject_Print(array_obj, stdout, 0);
//    printf("\n");

    PyArray_OutputConverter(array_obj, &ndarray);


    npy_intp* coordinates = calloc(1 + number_dimensions, sizeof(npy_intp));
    for(npy_intp i=0;i<dimension_lengths[0];i++) {
        bson_iter_next(&bsonit);
        coordinates[0] = i;
        int success = _load_scalar(&bsonit, coordinates, ndarray, 0, number_dimensions + 1);
        if(success == -1) {
            PyErr_SetString(BsonNumpyError, "item failed to load");
            return NULL;
        }
    }

    free(dimension_lengths);
    free(document);
    free(coordinates);
//    Py_INCREF(array_obj);
    return array_obj;
}

static PyMethodDef BsonNumpyMethods[] = {
    {"ndarray_to_bson", ndarray_to_bson, METH_VARARGS,
     "Convert an ndarray into a BSON byte string"},
    {"bson_to_ndarray", bson_to_ndarray, METH_VARARGS,
     "Convert BSON byte string into an ndarray"},
    {NULL, NULL, 0, NULL}        /* Sentinel */
};

PyMODINIT_FUNC
initbsonnumpy(void)
{
    PyObject* m;

    m = Py_InitModule("bsonnumpy", BsonNumpyMethods);
    if (m == NULL)
        return;

    BsonNumpyError = PyErr_NewException("bsonnumpy.error", NULL, NULL);
    Py_INCREF(BsonNumpyError);
    PyModule_AddObject(m, "error", BsonNumpyError);

    import_array();
}


int
main(int argc, char* argv[])
{
    printf("RUNNING MAIN");
    /* Pass argv[0] to the Python interpreter */
    Py_SetProgramName(argv[0]);

    /* Initialize the Python interpreter.  Required. */
    Py_Initialize();

    /* Add a static module */
    initbsonnumpy();
}

