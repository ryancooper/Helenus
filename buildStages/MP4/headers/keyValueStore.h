#ifndef AG_KV_STORE
#define AG_KV_STORE

#include <iostream>
#include <string>
#include <ctime>
#include <vector>
#include <map>
#include <list>

#include <boost/serialization/vector.hpp>
#include <boost/archive/text_oarchive.hpp>
#include <boost/archive/text_iarchive.hpp>
#include <boost/unordered_map.hpp>

#include "logger.h"
#include "errorCodes.h"
#include "utility.h"
#include "fileHandler.h"
#include "clt.h"
#include "Hash.h"
#include "coordinator.h"

#define KEY_REPLICATED 0
#define KEY_OWNED 1

#define MAX_HASH 255

using namespace std;

class Value {
	//the actual value
	string value;
	//the ownership status of the key, owned or replicated
	int owned; //1 means owner, 0 means replicated, -1 means undetermined (init value, should never be the value for a real key)

	public:
	/**
	 * Constructor
	 */
	Value (string valueString) {
		setValue(valueString);
		owned = -1;
	}

	/**
	 * [sets a particular value]
	 * @param valueString [the value that has to be set]
	 */
	void setValue(string valueString) {
		value = valueString;
	}

	/**
	 * [sets current machine as owner]
	 */
	void setAsOwner(){
		owned = 1;
	}

	/**
	 * [sets current machine as replica]
	 */
	void setAsReplica(){
		owned = 0;
	}

	/**
	 * [gets the string at this value object]
	 * @return [description]
	 */
	string getValue() {
		return value;
	}

	/**
	 * [is this store the owner of the key-value pair]
	 * @return [is owner]
	 */
	int isOwner() {
		return owned == 1;
	}

	/**
	 * [is this key value store the replica of the key value pair]
	 * @return [is replica]
	 */
	int isReplica() {
		return owned == 0;
	}
};


class KeyValueStore {

	//the hash map to store our keys
	boost::unordered_map<string,Value> keyValueStore;
	//the machine ID that is the owner of this key value store
	int machineID;
	//the logger object
	ErrorLog *logger;
	//the coordinator
	Coordinator *coordinator;
	//lock for operations
	pthread_mutex_t mutexsum;

	/**
	 * [checks for any preexisting errors before executing APIs]
	 * @param  errCode      [the error code. should be 0 if no error]
	 * @param  functionName [the name of the function requesting the check]
	 * @return              [status of the error code. SUCCESS if 0]
	 */
	int checkForPreExistingError(int *errCode, const char* functionName) {
		//bail if there is already an error
		if(*errCode > ERROR_IO_LOGIC) {
			string msg = "Unable to execute " + string(functionName) + ". A previous error with error code: " + Utility::intToString(*errCode) + " exists";
			logger->logError(ERROR_ALREADY_EXISTS, msg , errCode);
			*errCode = ERROR_ALREADY_EXISTS;
			return FAILURE;
		}

		return SUCCESS;
	}

	/**
	 * [inserts a key into the key value store]
	 * @param  key         [the key]
	 * @param  valueString [the value]
	 * @param  owned 	   [is this supposed to inserted as an owned key?]
	 * @param  errCode     [place to store errors]
	 * @return             [status of insert]
	 */
	int privateInsertKeyValue(string key, string valueString, int owned, int *errCode) {
		//check for preexisting error
		int status = checkForPreExistingError(errCode, __func__);
		if (status == FAILURE) {
			return status;
		}

		pthread_mutex_lock (&mutexsum);

		//make the value object
		Value value = Value(valueString);
		if(owned) {
			value.setAsOwner();
		} else {
			value.setAsReplica();
		}

		//try to look the key up first
		boost::unordered_map<string, Value>::iterator lookupEntry = privateLookupKey(key, errCode, 1);

		if(lookupEntry == keyValueStore.end()) { //key does not exist
			//insert
			status = int(keyValueStore.insert(make_pair(key, value)).second);
		} else if(owned) {//key exists and is owned. (if force insert, just ignore a reinsert attempt)
				string msg = "Tried to insert a key which already exists. Key is " + key;
				logger->logError(KEY_EXISTS, msg , errCode);
				*errCode = KEY_EXISTS;
				status = FAILURE;
		}

		pthread_mutex_unlock (&mutexsum);
		//error check
		if(status == FAILURE){
			string msg = "Inserting key failed. Key is " + key;
			logger->logError(INSERT_FAILED, msg , errCode);
		}

		return status;
	}

	/**
	 * [deletes the specified key]
	 * @param  key     [the key]
	 * @param  errCode [place to store error code]
	 * @return         [the status of delete]
	 */
	int privateDeleteKey(string key, int *errCode) {
		//check for preexisting error
		int status = checkForPreExistingError(errCode, __func__);
		if (status == FAILURE) {
			return status;
		}

		pthread_mutex_lock (&mutexsum);
		//delete the key
		int elementsErased = keyValueStore.erase(key);

		if (elementsErased == 0) {//none deleted?
			string msg = "Tried to delete a key which does not exist. Key is " + key;
			logger->logError(NO_SUCH_KEY, msg , errCode);
		} else if (elementsErased > 1) {//too many deleted? (should not happen!)
			string msg = "Found more than one matching key and deleted them all! Key is " + key;
			logger->logError(TOO_MANY_KEYS, msg, errCode);
		}
		pthread_mutex_unlock (&mutexsum);

		return status;
	}

	/**
	 * [update the value of a key]
	 * @param  key         [the key]
	 * @param  valueString [the new value]
	 * @param  errCode     [place to store error code]
	 * @return             [status of update]
	 */
	int privateUpdateKeyValue(string key, string valueString, int *errCode) {
		//check for preexisting error
		int status = checkForPreExistingError(errCode, __func__);
		if (status == FAILURE) {
			return status;
		}

		pthread_mutex_lock (&mutexsum);

		boost::unordered_map<string, Value>::iterator lookupEntry = privateLookupKey(key, errCode, 1);
		
		if(lookupEntry != keyValueStore.end()) { //key exists

			lookupEntry->second.setValue(valueString);
		} else { //key does not exist
			string msg = "Updating key failed. Check if key exists. If not, use insert. Key is " + key;
			logger->logError(UPDATE_FAILED, msg , errCode);
			*errCode = UPDATE_FAILED;
			status = FAILURE;
		}

		pthread_mutex_unlock (&mutexsum);

		return status;
	}

	/**
	 * [private lookup function that can be optionally asked to suppress errors]
	 * @param  key           [key to lookup]
	 * @param  errCode       [place to store error codes if any]
	 * @param  suppressError [if true, doesn't log an errors]
	 * @return               [the iterator in the hash map which points to the key]
	 */
	boost::unordered_map<string, Value>::iterator privateLookupKey(string key, int *errCode, int suppressError) {
		boost::unordered_map<string, Value>::iterator entry = keyValueStore.end();

		//check for preexisting error
		int status = checkForPreExistingError(errCode, __func__);
		if (status == FAILURE) {
			return entry;
		}

		//try to look the key up
		entry = keyValueStore.find(key);

		//is the key there?
		if(entry == keyValueStore.end()) {
			//no such key
			if(!suppressError) {
				string msg = "Tried to lookup a key which does not exist. Key is " + key;
				logger->logError(NO_SUCH_KEY, msg , errCode);
			}
		}

		return entry;

	}

	/**
	 * [gets the hash of a key]
	 * @param  num [the key]
	 * @return     [the hash]
	 */
	int getHash(string num) {
		return Hash::calculateKeyHash(num);
	}

	/**
	 * [builds a command to perform a certain operation]
	 * @param  operation [the operation to be performed]
	 * @param  key       [the key]
	 * @param  value     [the value]
	 * @return           [the built command]
	 */
	string buildCommand(string operation, string key, string value) {
		//builds a well formed command of the form operation(key,value)
		string command = operation + "(" + key;
		if(operation == UPDATE_KEY || operation == INSERT_KEY || operation == FORCE_UPDATE_KEY || operation == FORCE_INSERT_KEY) {
			command += ",";
			command += value;
		}
		command += ")";

		return command;
	}

	/**
	 *	[builds commands to perform an operation on a set of keys]
	 * @param  operation [the operation to be performed]
	 * @param  keys      [the set of keys]
	 * @return           [the built commands]
	 */
	vector<string> getCommands(string operation, vector<string> keys, int *errCode) {
		vector<string> commands;
		string command = "";
		string value = "";
		string key;

		//iterate over the keys and form well formed commands for each of them
		for(int i = 0; i < keys.size(); i++){
			key = keys[i];
			//lookup the value
			value = lookupKey(key, errCode);
			//build command
			command = buildCommand(operation, key, value);
			//add to list
			commands.push_back(command);
		}

		return commands;
	} 

	/**
	 * [gets all keys owned by this machine]
	 * @param  errCode [space to store error code if any]
	 * @return         [the list of keys owned by this machine]
	 */
	vector<string> getOwnedKeys(int *errCode) {
		vector<string> keys;
		for(boost::unordered_map<string, Value>::iterator it = keyValueStore.begin(); it != keyValueStore.end(); it++) {
			if(it->second.isOwner()) {
				keys.push_back(it->first);
			}
		}

		return keys;
	}

	/**
	 * [gets all keys replicated by this machine]
	 * @param  errCode [space to store error code if any]
	 * @return         [the list of keys replicated by this machine]
	 */
	vector<string> getReplicatedKeys(int *errCode) {
		vector<string> keys;
		for(boost::unordered_map<string, Value>::iterator it = keyValueStore.begin(); it != keyValueStore.end(); it++) {
			if(it->second.isReplica()) {
				keys.push_back(it->first);
			}
		}

		return keys;
	}

	/**
	 * [changes replicated key to owned key]
	 * @param  key     [the key]
	 * @param  errCode [place to store error]
	 * @return         [status of request]
	 */
	int changeKeyToOwned(string key, int *errCode) {
		//check for preexisting error
		int status = checkForPreExistingError(errCode, __func__);
		if (status == FAILURE) {
			return status;
		}

		pthread_mutex_lock (&mutexsum);

		boost::unordered_map<string, Value>::iterator lookupEntry = privateLookupKey(key, errCode, 1);
		
		if(lookupEntry != keyValueStore.end()) { //key exists
			//set as the owner
			lookupEntry->second.setAsOwner();
		} else { //key does not exist
			string msg = "Updating ownership status of key failed. Check if key exists. Key is " + key;
			logger->logError(UPDATE_FAILED, msg , errCode);
			*errCode = UPDATE_FAILED;
			status = FAILURE;
		}

		pthread_mutex_unlock (&mutexsum);

		return status;
	}


	public:

        list<string> keyToValueWriteCache;
        list<string> keyToValueReadCache;

	/**
	 * Constructor. Supply ID of machine which will own the key value store and the logger object of the machine
	 */
	KeyValueStore(int ID, ErrorLog *logObject, Coordinator *coord) {
		machineID = ID;
		logger = logObject;
		coordinator = coord;

		/* Initialize the mutex */
		pthread_mutex_init(&mutexsum, NULL);
	}

	
	/**
	 * [public interface for normal insert]
	 * @param  key         [the key]
	 * @param  valueString [the value]
	 * @param  errCode     [place to store errors]
	 * @return             [status of insert]
	 */
	int insertKeyValue(string key, string valueString, int *errCode) {
		return privateInsertKeyValue(key, valueString, KEY_OWNED, errCode);
	}


	/**
	 * [public interface for force insert]
	 * @param  key         [the key]
	 * @param  valueString [the value]
	 * @param  errCode     [place to store errors]
	 * @return             [status of insert]
	 */
	int forceInsertKeyValue(string key, string valueString, int *errCode) {
		return privateInsertKeyValue(key, valueString, KEY_REPLICATED, errCode);
	}

	/**
	 * [public interface for normal delete]
	 * @param  key     [the key]
	 * @param  errCode [place to store error code]
	 * @return         [the status of delete]
	 */
	int deleteKey(string key, int *errCode) {
		return privateDeleteKey(key, errCode);
	}

	/**
	 * [public interface for force delete]
	 * @param  key     [the key]
	 * @param  errCode [place to store error code]
	 * @return         [the status of delete]
	 */
	int forceDeleteKey(string key, int *errCode) {
		return privateDeleteKey(key, errCode);
	}

	/**
	 * [public interface for normal update of key]
	 * @param  key         [the key]
	 * @param  valueString [the new value]
	 * @param  errCode     [place to store error code]
	 * @return             [status of update]
	 */
	int updateKeyValue(string key, string valueString, int *errCode) {
		return privateUpdateKeyValue(key, valueString, errCode);
	}

	/**
	 * [public interface for force update of key]
	 * @param  key         [the key]
	 * @param  valueString [the new value]
	 * @param  errCode     [place to store error code]
	 * @return             [status of update]
	 */
	int forceUpdateKeyValue(string key, string valueString, int *errCode) {
		return privateUpdateKeyValue(key, valueString, errCode);
	}


	/**
	 * [public interface of lookup key. does not allow suppressing of errors]
	 * @param  key     [key to lookup]
	 * @param  errCode [place to store error code]
	 * @return         [the value of key]
	 */
	string lookupKey(string key, int *errCode) {
		string value = "";
		//get the entry
		boost::unordered_map<string, Value>::iterator lookupEntry = privateLookupKey(key, errCode, 0);
		//if lookup did not fail, extract the value
		if(lookupEntry != keyValueStore.end()) {
			 value = lookupEntry->second.getValue();
		}

		return value;
	}

	/**
	 * [public interface of force lookup key. does not allow suppressing of errors]
	 * @param  key     [key to lookup]
	 * @param  errCode [place to store error code]
	 * @return         [the value of key]
	 */
	string forceLookupKey(string key, int *errCode) {
		return lookupKey(key, errCode);
	}
	/**
	 * [returns all entries in the key value store]
	 * @param  errCode [place to store errors]
	 * @return         [all the entries as one string]
	 */
	string returnAllEntries(int *errCode) {
		string value = "";
		//check for preexisting error
		int status = checkForPreExistingError(errCode, __func__);
		if (status == FAILURE) {
			return value;
		}

		//build all the entries in the store as key:value
		int i = 0;
		for(boost::unordered_map<string, Value>::iterator it = keyValueStore.begin(); it != keyValueStore.end(); it++) {
			value += it->second.isOwner() ? "(Owned) " : "(Replicated) ";
			value += it->first;
			value += ":";
			value += it->second.getValue();
			value += "\n";
			i++;
		}

		cout<<"Entries: "<<i<<endl;
		return value;
	}

	/**
	 * [prints all entries in the key value store]
	 * @param errCode [place to store error code]
	 */
	void show(int *errCode) {
		//check for preexisting error
		int status = checkForPreExistingError(errCode, __func__);
		if (status == FAILURE) {
			cout<<"Unable to show entries in the key value store. Check log for details"<<endl;
		}

		//get and print entries
		string entries = returnAllEntries(errCode);
		if (entries == "") {
			cout<<"No entries in key value store or unable to show entries. Check the log to see if there are any errors"<<endl;
		} else {
			cout<<"The contents of the key value store at machine "<<machineID<<" are: "<<endl;
			cout<<entries<<endl;
		}
	}


	/*************************EVENT HANDLERS**********************************/

	/**********************************JOIN HANDLING FUNCTIONS***************************************/
	/**
	 * [builds commands for keys to be sent when a machine joins]
	 * @param  rangeStart 	 		[new machine owned range start]
	 * @param  rangeEnd 	 		[new machine owned range end]
	 * @param  deleteCommands 	 	[space to store delete commands that get sent to replica]
	 * @param  errCode 	 			[space to store error code]
	 * @return           			[the built commands]
	 */
	vector<string> getCommandsToTransferOwnedKeysAtJoin(int rangeStart, int rangeEnd, vector<string> *deleteCommands, int *errCode) {
		vector <string>commands;
		if(rangeStart > rangeEnd) {
			//if this is the case, then the joinee is the first machine on the ring
			vector<string> commandsLower = getCommandsToTransferOwnedKeysAtJoin(0, rangeEnd, deleteCommands, errCode);
			vector<string> commandsHigher = getCommandsToTransferOwnedKeysAtJoin(rangeStart, MAX_HASH + 1, deleteCommands, errCode);
			//combine
			commands = Utility::combineVectors(commandsLower, commandsHigher);
		} else {
			//get all the keys owned by this store
			vector<string> ownedKeys = getOwnedKeys(errCode);
			//comparison to see which keys need to go to the new joinee
			//goes through all of its keys to see if there are any that are lesser than the hash of the new machine (on the ring)
			////pick out keys which are in the range
			vector<string> selectedKeys;
			for(int i =0 ; i < ownedKeys.size() ; i++) {
				string key = ownedKeys[i];
				int keyHash = getHash(key);
				if(rangeStart <= keyHash &&  keyHash < rangeEnd) {
					selectedKeys.push_back(key);
				}
			}
			//get well formed commands for the keys selected, to send to the new machine
			commands = getCommands(INSERT_KEY, selectedKeys, errCode);
			//get fdelete commands to send to second replica for these keys
			*deleteCommands = getCommands(FORCE_DELETE_KEY, selectedKeys, errCode);
			
			//delete from this store
			for(int i=0; i < selectedKeys.size(); i++) {
				privateDeleteKey(selectedKeys[i], errCode);
			}
		}

		return commands;
	} 

	/**
	 * [deletes any extra replicated keys]
	 * @param  deleteRangeStart [range start]
	 * @param  deleteRangeEnd   [range end]
	 * @param  errCode          [space to store error code]
	 * @return                  [the number of deleted keys]
	 */
	int deleteReplicatedKeys(int deleteRangeStart, int deleteRangeEnd, int *errCode) {
		int deleted = 0;
		if(deleteRangeStart >= 0 && deleteRangeEnd >= 0) {
			if(deleteRangeStart > deleteRangeEnd) {
				//if the owner machine of these range of keys is the first machine, split the range into two
				deleted += deleteReplicatedKeys(0, deleteRangeEnd, errCode);
				deleted += deleteReplicatedKeys(deleteRangeStart, MAX_HASH + 1, errCode);
			} else {
				//get all the keys for which this store is replica
				vector<string> allReplicatedKeys = getReplicatedKeys(errCode);
				
				//pick out keys which are in the delete range and delete them (because this machine no longer needs to replicate them)
				for(int i =0 ; i < allReplicatedKeys.size() ; i++) {
					string key = allReplicatedKeys[i];
					int keyHash = getHash(key);
					if(deleteRangeStart <= keyHash &&  keyHash < deleteRangeEnd) {
						deleted++;
						privateDeleteKey(key, errCode);
					}
				}
			}
		}

		return deleted;
	}

	/**
	 * [gets commands to replicate owned keys on another machine]
	 * @param  errCode          [space to store error code]
	 * @return                  [the commands]
	 */
	vector<string> getCommandsToReplicateOwnedKeys(int *errCode) {
		//get all the keys owned by this store
		vector<string> ownedKeys = getOwnedKeys(errCode);
	
		//create force deletes in case the machine has a stale copy
		vector<string> deleteCommands = getCommands(FORCE_DELETE_KEY, ownedKeys, errCode);
		//create force inserts for replication
		vector<string> insertCommands = getCommands(FORCE_INSERT_KEY, ownedKeys, errCode);

		return Utility::combineVectors(deleteCommands, insertCommands);
	}

	/******************FAILURE HANDLING FUNCTIONS**********************************/

	/*
	 * [marks some keys as newly owned after failure and gets commands to replicate them in a replica]
	 * @param  rangeStart [range start]
	 * @param  rangeEnd   [range end]
	 * @param  errCode    [space to store error code]
	 * @return            [the commands]
	 */
	vector<string> getCommandsToReplicateNewOwnedKeysAtFailOrLeave(int rangeStart, int rangeEnd, int *errCode) {
		vector<string> commands;
		int status = 1;
		if(rangeStart >= 0 && rangeEnd >= 0) {
			if(rangeStart > rangeEnd) {
				//if the owner machine of these range of keys is the first machine, split the range into two
				vector<string> commandsLower = getCommandsToReplicateNewOwnedKeysAtFailOrLeave(0, rangeEnd, errCode);
				vector<string> commandsHigher = getCommandsToReplicateNewOwnedKeysAtFailOrLeave(rangeStart, MAX_HASH + 1, errCode);
				//combine
				commands = Utility::combineVectors(commandsLower, commandsHigher);
			} else {
				//get all the keys for which this store is replica
				vector<string> allReplicatedKeys = getReplicatedKeys(errCode);

				//pick out keys which are in the range
				vector<string> selectedReplicatedKeys;
				for(int i =0 ; i < allReplicatedKeys.size() ; i++) {
					string key = allReplicatedKeys[i];
					int keyHash = getHash(key);
					if(rangeStart <= keyHash &&  keyHash < rangeEnd) {
						//mark it as owned
						status = changeKeyToOwned(key, errCode);
						selectedReplicatedKeys.push_back(key);
					}
				}

				vector<string> forceDeleteCommands = getCommands(FORCE_DELETE_KEY, selectedReplicatedKeys, errCode);
				vector<string> forceInsertCommands = getCommands(FORCE_INSERT_KEY, selectedReplicatedKeys, errCode);

				commands = Utility::combineVectors(forceDeleteCommands, forceInsertCommands);
			}
		}

		return commands;
	}

	/**
	 * [shows the last 10 reads and writes]
	 */
    void showCachedEntries() {
        list<string>::iterator cacheitr;
        if(keyToValueReadCache.size() == 0)
                cout << "Read Cache Empty " << endl;
        else {
			cout << "Last 10 Reads" << endl;
        	for(cacheitr = keyToValueReadCache.begin();
        	    cacheitr != keyToValueReadCache.end();
        	    cacheitr++) {
                	cout << *cacheitr << endl;
        	}
        }
        if(keyToValueWriteCache.size() == 0)
                cout << endl << "Write Cache Empty " << endl;
        else {
        	cout << endl << "Last 10 Writes" << endl;
        	for(cacheitr = keyToValueWriteCache.begin();
        	    cacheitr != keyToValueWriteCache.end();
        	    cacheitr++) {
        	        cout << *cacheitr << endl;
        	}
        }
    }

};

#endif
