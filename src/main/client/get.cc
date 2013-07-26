/*******************************************************************************
 * Copyright 2013 Aerospike Inc.
 *
 * Permission is hereby granted, free of charge, to any person obtaining a copy 
 * of this software and associated documentation files (the "Software"), to 
 * deal in the Software without restriction, including without limitation the 
 * rights to use, copy, modify, merge, publish, distribute, sublicense, and/or 
 * sell copies of the Software, and to permit persons to whom the Software is 
 * furnished to do so, subject to the following conditions:
 * 
 * The above copyright notice and this permission notice shall be included in 
 * all copies or substantial portions of the Software.
 * 
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
 * IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
 * FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
 * AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
 * LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING 
 * FROM, OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS
 * IN THE SOFTWARE.
 ******************************************************************************/

extern "C" {
	#include <aerospike/aerospike.h>
	#include <aerospike/aerospike_key.h>
	#include <aerospike/as_config.h>
	#include <aerospike/as_key.h>
	#include <aerospike/as_record.h>
	#include <aerospike/as_record_iterator.h>
}

#include <node.h>
#include <cstdlib>
#include <unistd.h>

#include "../client.h"
#include "../util/async.h"
#include "../util/conversions.h"
#include "../util/log.h"

using namespace v8;

/*******************************************************************************
 *	TYPES
 ******************************************************************************/

/**
 *	AsyncData — Data to be used in async calls.
 */
typedef struct AsyncData {
	aerospike * as;
	as_error err;
	as_key key;
	as_record rec;
	Persistent<Function> callback;
	bool get_all_bins;
	int num_bins;
	char** bins;
} AsyncData;

/*******************************************************************************
 *	FUNCTIONS
 ******************************************************************************/

/**
 *	prepare() — Function to prepare AsyncData, for use in `execute()` and `respond()`.
 *  
 *	This should only keep references to V8 or V8 structures for use in 
 *	`respond()`, because it is unsafe for use in `execute()`.
 */
static void * prepare(const Arguments& args)
{
	// The current scope of the function
	HandleScope scope;

	AerospikeClient * client = ObjectWrap::Unwrap<AerospikeClient>(args.This());

	// Build the async data
	AsyncData *	data = new AsyncData;
	data->as = &client->as;

	// Local variables
	as_key *	key = &data->key;
	as_record *	rec = &data->rec;

	if ( args[0]->IsArray() ) {
		Local<Array> arr = Local<Array>::Cast(args[0]);
		key_from_jsarray(key, arr);
	}
	else if ( args[0]->IsObject() ) {
		key_from_jsobject(key, args[0]->ToObject());
	}

	as_record_init(rec, 0);

	// To select the values of given bin, not complete record.
	if ( args.Length() == 3  && args[1]->IsArray() ) {
	
		Local<Array> barray = Local<Array>::Cast(args[1]);	
		data->get_all_bins = false;
		int num_bins = barray->Length();
		data->num_bins = num_bins;
		data->bins = (char **)calloc(sizeof(char *), num_bins+1);
		for (int i=0; i < num_bins; i++) {
			Local<Value> bname = barray->Get(i);
			data->bins[i] = (char*) malloc(AS_BIN_NAME_MAX_SIZE);
			strncpy(data->bins[i],  *String::Utf8Value(bname), AS_BIN_NAME_MAX_SIZE);
		}
		// The last entry should be NULL because we are passing to aerospike_key_select
		data->bins[num_bins] = NULL;
		
	}
	else {
		data->get_all_bins = true;
	}

	data->callback = Persistent<Function>::New(Local<Function>::Cast(args[args.Length() - 1]));
		
	return data;
}
/**
 *	execute() — Function to execute inside the worker-thread.
 *  
 *	It is not safe to access V8 or V8 data structures here, so everything
 *	we need for input and output should be in the AsyncData structure.
 */
static void execute(uv_work_t * req)
{
	// Fetch the AsyncData structure
	AsyncData * data = reinterpret_cast<AsyncData *>(req->data);

	// Data to be used.
	aerospike *	as	= data->as;
	as_error *	err	= &data->err;
	as_key *	key	= &data->key;
	as_record *	rec	= &data->rec;
		

	// Invoke the blocking call.
	// The error is handled in the calling JS code.
	if (data->get_all_bins == true) {	
		aerospike_key_get(as, err, NULL, key, &rec);
	}

	else {
		aerospike_key_select(as, err, NULL, key, (const char **)data->bins, &rec);	
	}
	
}

/**
 *	respond() — Function to be called after `execute()`. Used to send response
 *  to the callback.
 *  
 *	This function will be run inside the main event loop so it is safe to use 
 *	V8 again. This is where you will convert the results into V8 types, and 
 *	call the callback function with those results.
 */
static void respond(uv_work_t * req, int status)
{
	// Scope for the callback operation.
	HandleScope scope;

	// Fetch the AsyncData structure
	AsyncData *	data	= reinterpret_cast<AsyncData *>(req->data);
	as_error *	err		= &data->err;
	as_key *	key		= &data->key;
	as_record *	rec		= &data->rec;

	// Build the arguments array for the callback
	Handle<Value> argv[] = {
		error_to_jsobject(err),
		recordbins_to_jsobject(rec),
		recordmeta_to_jsobject(rec),
		key_to_jsobject(key)
	};

	// Surround the callback in a try/catch for safety
	TryCatch try_catch;

	// Execute the callback.
	data->callback->Call(Context::GetCurrent()->Global(), 4, argv);

	// Process the exception, if any
	if ( try_catch.HasCaught() ) {
		node::FatalException(try_catch);
	}
	
	// Dispose the Persistent handle so the callback
	// function can be garbage-collected
	data->callback.Dispose();

	// clean up any memory we allocated
	
	as_key_destroy(key);
	as_record_destroy(rec);

	delete data;
	delete req;
}

/*******************************************************************************
 *  OPERATION
 ******************************************************************************/

/**
 *	The 'get()' Operation
 */
Handle<Value> AerospikeClient::Get(const Arguments& args)
{
	return async_invoke(args, prepare, execute, respond);
}
