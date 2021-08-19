/*
 * BSD 2-Clause License
 *
 * Copyright (c) 2021, Hewlett Packard Enterprise
 * All rights reserved.
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions are met:
 *
 * 1. Redistributions of source code must retain the above copyright notice, this
 *    list of conditions and the following disclaimer.
 *
 * 2. Redistributions in binary form must reproduce the above copyright notice,
 *    this list of conditions and the following disclaimer in the documentation
 *    and/or other materials provided with the distribution.
 *
 * THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS "AS IS"
 * AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE
 * IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE ARE
 * DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT HOLDER OR CONTRIBUTORS BE LIABLE
 * FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL
 * DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR
 * SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER
 * CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY,
 * OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE
 * OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.
 */

#include "rediscluster.h"

using namespace SmartRedis;

// RedisCluster constructor
RedisCluster::RedisCluster() : RedisServer() {
    std::string address_port = this->_get_ssdb();
    this->_connect(address_port);
    this->_map_cluster();
}

// RedisCluster constructor. Uses address provided to constructor instead of
// environment variables
RedisCluster::RedisCluster(std::string address_port) : RedisServer()
{
    this->_connect(address_port);
    this->_map_cluster();
}

// RedisCluster destructor
RedisCluster::~RedisCluster()
{
    if (this->_redis_cluster != NULL) {
        delete this->_redis_cluster;
        this->_redis_cluster = NULL;
    }
}

// Run a single-key or single-hash slot Command on the server
CommandReply RedisCluster::run(Command& cmd)
{
    // Preprend the target database to the command
    std::string db_prefix;
    if (this->is_addressable(cmd.get_address(), cmd.get_port()))
        db_prefix = this->_address_node_map.at(cmd.get_address() + ":"
                    + std::to_string(cmd.get_port()))->prefix;
    else if (cmd.has_keys())
        db_prefix = this->_get_db_node_prefix(cmd);
    else
        throw std::runtime_error("Redis has failed to find database");
    std::string_view sv_prefix(db_prefix.data(), db_prefix.size());

    // Execute the commmand
    Command::iterator cmd_fields_start = cmd.begin();
    Command::iterator cmd_fields_end = cmd.end();
    CommandReply reply;

    int n_trials = 100;
    for (int trial = 0; trial < n_trials; trial++) {
        bool do_sleep = false;
        try {
            sw::redis::Redis db = this->_redis_cluster->redis(sv_prefix, false);
            reply = db.command(cmd_fields_start, cmd_fields_end);
            if (reply.has_error() == 0)
                return reply;
            break;
        }
        catch (sw::redis::IoError &e) {
            do_sleep = true;
        }
        catch (sw::redis::ClosedError &e) {
            do_sleep = true;
        }
        catch (std::exception& e) {
            throw std::runtime_error(e.what());
        }
        catch (...) {
            throw std::runtime_error("A non-standard exception encountered "\
                                     "during command " + cmd.first_field() +
                                     " execution.");
        }

        // Sleep before the next attempt
        if (do_sleep) {
            std::this_thread::sleep_for(std::chrono::seconds(2));
            do_sleep = false;
        }
    }

    // We should only get here on an error response
    if (reply.has_error() > 0)
        reply.print_reply_error();
    throw std::runtime_error("Redis failed to execute command: " +
                                cmd.first_field());
    return reply; // never reached
}

// Run multiple single-key or single-hash slot Command on the server.
// Each Command in the CommandList is run sequentially
CommandReply RedisCluster::run(CommandList& cmds)
{
    CommandList::iterator it = cmds.begin();
    CommandReply reply;
    for ( ; it != cmds.end(); it++) {
        reply = this->run(**it);
        if (reply.has_error() > 0) {
            throw std::runtime_error("Subcommand " + (*it)->first_field() +
                                     " failed");
        }
    }

    // Done
    return reply;
}

// Check if a model or script key exists in the database
bool RedisCluster::model_key_exists(const std::string& key)
{
    // Add model prefix to the key
    DBNode* node = &(this->_db_nodes[0]);
    if (node == NULL)
        return false;
    std::string prefixed_key = '{' + node->prefix + "}." + key;

    // And perform key existence check
    return this->key_exists(prefixed_key);
}

// Check if a key exists in the database
bool RedisCluster::key_exists(const std::string& key)
{
    // Build the command
    Command cmd;
    cmd.add_field("EXISTS");
    cmd.add_field(key, true);

    // Run it
    CommandReply reply = this->run(cmd);
    if (reply.has_error() > 0)
        throw std::runtime_error("Error encountered while checking "\
                                 "for existence of key " + key);
    return (bool)reply.integer();
}

// Check if a key exists in the database
bool RedisCluster::is_addressable(const std::string& address,
                                  const uint64_t& port)
{
    std::string addr = address + ":" + std::to_string(port);
    return this->_address_node_map.find(addr) != this->_address_node_map.end();
}

// Put a Tensor on the server
CommandReply RedisCluster::put_tensor(TensorBase& tensor)
{
    // Build the command
    Command cmd;
    cmd.add_field("AI.TENSORSET");
    cmd.add_field(tensor.name(), true);
    cmd.add_field(tensor.type_str());
    cmd.add_fields(tensor.dims());
    cmd.add_field("BLOB");
    cmd.add_field_ptr(tensor.buf());

    // Run it
    return this->run(cmd);
}

// Get a Tensor from the server
CommandReply RedisCluster::get_tensor(const std::string& key)
{
    // Build the command
    Command cmd;
    cmd.add_field("AI.TENSORGET");
    cmd.add_field(key, true);
    cmd.add_field("META");
    cmd.add_field("BLOB");

    // Run it
    return this->run(cmd);
}

// Rename a tensor in the database
CommandReply RedisCluster::rename_tensor(const std::string& key,
                                         const std::string& new_key)
{
    // Check wehether we have to switch hash slots
    uint16_t key_hash_slot = this->_get_hash_slot(key);
    uint16_t new_key_hash_slot = this->_get_hash_slot(new_key);

    // If not, we can use a simple RENAME command
    CommandReply reply;
    if (key_hash_slot == new_key_hash_slot) {
        // Build the command
        Command cmd;
        cmd.add_field("RENAME");
        cmd.add_field(key, true);
        cmd.add_field(new_key, true);

        // Run it
        reply = this->run(cmd);
    }

    // Otherwise we need to clone the tensor and then nuke the old one
    else {
        this->copy_tensor(key, new_key);
        reply = this->delete_tensor(key);
    }

    // Done
    return reply;
}

// Delete a tensor in the database
CommandReply RedisCluster::delete_tensor(const std::string& key)
{
    // Build the command
    Command cmd;
    cmd.add_field("UNLINK");
    cmd.add_field(key, true);

    // Run it
    return this->run(cmd);
}

// Copy a tensor from the source key to the destination key
CommandReply RedisCluster::copy_tensor(const std::string& src_key,
                                       const std::string& dest_key)
{
    //TODO can we do COPY for same hash slot or database (only for redis 6.2)?

    // Build the GET command
    Command cmd_get;
    cmd_get.add_field("AI.TENSORGET");
    cmd_get.add_field(src_key, true);
    cmd_get.add_field("META");
    cmd_get.add_field("BLOB");

    // Run the GET command
    CommandReply cmd_get_reply = this->run(cmd_get);
    if (cmd_get_reply.has_error() > 0)
        throw std::runtime_error("Failed to find tensor " + src_key);

    // Decode the tensor
    std::vector<size_t> dims =
        CommandReplyParser::get_tensor_dims(cmd_get_reply);
    std::string_view blob =
        CommandReplyParser::get_tensor_data_blob(cmd_get_reply);
    TensorType type =
        CommandReplyParser::get_tensor_data_type(cmd_get_reply);

    // Build the PUT command
    Command cmd_put;
    cmd_put.add_field("AI.TENSORSET");
    cmd_put.add_field(dest_key, true);
    cmd_put.add_field(TENSOR_STR_MAP.at(type));
    cmd_put.add_fields(dims);
    cmd_put.add_field("BLOB");
    cmd_put.add_field_ptr(blob);

    // Run the PUT command
    return this->run(cmd_put);
}

// Copy a vector of tensors from source keys to destination keys
CommandReply RedisCluster::copy_tensors(const std::vector<std::string>& src,
                                        const std::vector<std::string>& dest)
{
    // Make sure vectors are the same length
    if (src.size() != dest.size()) {
        throw std::runtime_error("differing size vectors "\
                                 "passed to copy_tensors");
    }

    // Copy tensors one at a time. We only need to check one iterator
    // for reaching the end since we know from above that they are the
    // same length
    std::vector<std::string>::const_iterator it_src = src.cbegin();
    std::vector<std::string>::const_iterator it_dest = dest.cbegin();
    CommandReply reply;
    for ( ; it_src != src.cend(); it_src++, it_dest++) {
        reply = this->copy_tensor(*it_src, *it_dest);
        if (reply.has_error() > 0) {
            throw std::runtime_error("tensor copy failed");
        }

    }

    // Done
    return reply;
}

// Set a model from a string buffer in the database for future execution
CommandReply RedisCluster::set_model(const std::string& model_name,
                                     std::string_view model,
                                     const std::string& backend,
                                     const std::string& device,
                                     int batch_size,
                                     int min_batch_size,
                                     const std::string& tag,
                                     const std::vector<std::string>& inputs,
                                     const std::vector<std::string>& outputs)
{
    std::vector<DBNode>::const_iterator node = this->_db_nodes.cbegin();
    CommandReply reply;
    for ( ; node != this->_db_nodes.cend(); node++) {
        // Build the node prefix
        std::string prefixed_key = "{" + node->prefix + "}." + model_name;

        // Build the MODELSET commnd
        Command cmd;
        cmd.add_field("AI.MODELSET");
        cmd.add_field(prefixed_key, true);
        cmd.add_field(backend);
        cmd.add_field(device);

        // Add optional fields as requested
        if (tag.size() > 0) {
            cmd.add_field("TAG");
            cmd.add_field(tag);
        }
        if (batch_size > 0) {
            cmd.add_field("BATCHSIZE");
            cmd.add_field(std::to_string(batch_size));
        }
        if (min_batch_size > 0) {
            cmd.add_field("MINBATCHSIZE");
            cmd.add_field(std::to_string(min_batch_size));
        }
        if( inputs.size() > 0) {
            cmd.add_field("INPUTS");
            cmd.add_fields(inputs);
        }
        if (outputs.size() > 0) {
            cmd.add_field("OUTPUTS");
            cmd.add_fields(outputs);
        }
        cmd.add_field("BLOB");
        cmd.add_field_ptr(model);

        // Run the command
        reply = this->run(cmd);
        if (reply.has_error() > 0) {
            throw std::runtime_error("SetModel failed for node " + node->name);
        }
    }

    // Done
    return reply;
}

// Set a script from a string buffer in the database for future execution
CommandReply RedisCluster::set_script(const std::string& key,
                                      const std::string& device,
                                      std::string_view script)
{
    CommandReply reply;
    std::vector<DBNode>::const_iterator node = this->_db_nodes.cbegin();
    for ( ; node != this->_db_nodes.cend(); node++) {
        // Build the node prefix
        std::string prefix_key = "{" + node->prefix + "}." + key;

        // Build the SCRIPTSET command
        Command cmd;
        cmd.add_field("AI.SCRIPTSET");
        cmd.add_field(prefix_key, true);
        cmd.add_field(device);
        cmd.add_field("SOURCE");
        cmd.add_field_ptr(script);

        // Run the command
        reply = this->run(cmd);
        if (reply.has_error() > 0) {
            throw std::runtime_error("SetModel failed for node " + node->name);
        }
    }

    // Done
    return reply;
}

// Run a model in the database using the specificed input and output tensors
CommandReply RedisCluster::run_model(const std::string& key,
                                     std::vector<std::string> inputs,
                                     std::vector<std::string> outputs)
{
    /*  For this version of run model, we have to copy all
        input and output tensors, so we will randomly select
        a model.  We can't use rand, because MPI would then
        have the same random number across all ranks.  Instead
        We will choose it based on the db of the first input tensor.
    */

    uint16_t hash_slot = this->_get_hash_slot(inputs[0]);
    uint16_t db_index = this->_get_dbnode_index(hash_slot, 0,
                                                this->_db_nodes.size()-1);
    DBNode* db = &(this->_db_nodes[db_index]);
    if (db == NULL) {
        throw std::runtime_error("Missing DB node found in run_model");
    }

    // Generate temporary names so that all keys go to same slot
    std::vector<std::string> tmp_inputs = _get_tmp_names(inputs, db->prefix);
    std::vector<std::string> tmp_outputs = _get_tmp_names(outputs, db->prefix);

    // Copy all input tensors to temporary names to align hash slots
    this->copy_tensors(inputs, tmp_inputs);

    // Build the MODELRUN command
    std::string model_name = "{" + db->prefix + "}." + std::string(key);
    Command cmd;
    cmd.add_field("AI.MODELRUN");
    cmd.add_field(model_name, true);
    cmd.add_field("INPUTS");
    cmd.add_fields(tmp_inputs);
    cmd.add_field("OUTPUTS");
    cmd.add_fields(tmp_outputs);

    // Run it
    CommandReply reply = this->run(cmd);
    if (reply.has_error() > 0) {
        std::string error("run_model failed for node ");
        error += db_index;
        throw std::runtime_error(error);
    }

    // Store the outputs back to the database
    this->copy_tensors(tmp_outputs, outputs);

    // Clean up the temp keys
    std::vector<std::string> keys_to_delete;
    keys_to_delete.insert(keys_to_delete.end(),
                            tmp_outputs.begin(),
                            tmp_outputs.end());
    keys_to_delete.insert(keys_to_delete.end(),
                            tmp_inputs.begin(),
                            tmp_inputs.end());
    this->_delete_keys(keys_to_delete);

    // Done
    return reply;
}

// Run a script function in the database using the specificed input
// and output tensors
CommandReply RedisCluster::run_script(const std::string& key,
                                      const std::string& function,
                                      std::vector<std::string> inputs,
                                      std::vector<std::string> outputs)
{
    // Locate the DB node for the script
    uint16_t hash_slot = this->_get_hash_slot(inputs[0]);
    uint16_t db_index = this->_get_dbnode_index(hash_slot, 0,
                                                this->_db_nodes.size()-1);
    DBNode* db = &(this->_db_nodes[db_index]);
    if (db == NULL) {
        throw std::runtime_error("Missing DB node found in run_script");
    }

    // Generate temporary names so that all keys go to same slot
    std::vector<std::string> tmp_inputs = _get_tmp_names(inputs, db->prefix);
    std::vector<std::string> tmp_outputs = _get_tmp_names(outputs, db->prefix);

    // Copy all input tensors to temporary names to align hash slots
    this->copy_tensors(inputs, tmp_inputs);
    std::string script_name = "{" + db->prefix + "}." + std::string(key);

    // Build the SCRIPTRUN command
    Command cmd;
    CommandReply reply;
    cmd.add_field("AI.SCRIPTRUN");
    cmd.add_field(script_name, true);
    cmd.add_field(function);
    cmd.add_field("INPUTS");
    cmd.add_fields(tmp_inputs);
    cmd.add_field("OUTPUTS");
    cmd.add_fields(tmp_outputs);

    // Run it
    reply = this->run(cmd);
    if (reply.has_error() > 0) {
        std::string error("run_model failed for node ");
        error += db_index;
        throw std::runtime_error(error);
    }

    // Store the output back to the database
    this->copy_tensors(tmp_outputs, outputs);

    // Clean up temp keys
    std::vector<std::string> keys_to_delete;
    keys_to_delete.insert(keys_to_delete.end(),
                            tmp_outputs.begin(),
                            tmp_outputs.end());
    keys_to_delete.insert(keys_to_delete.end(),
                            tmp_inputs.begin(),
                            tmp_inputs.end());
    this->_delete_keys(keys_to_delete);

    // Done
    return reply;
}

// Retrieve the model from the database
CommandReply RedisCluster::get_model(const std::string& key)
{
    // Build the node prefix
    std::string prefixed_str = "{" + this->_db_nodes[0].prefix + "}." + key;

    // Build the MODELGET command
    Command cmd;
    cmd.add_field("AI.MODELGET");
    cmd.add_field(prefixed_str, true);
    cmd.add_field("BLOB");

    // Run it
    return this->run(cmd);
}

// Retrieve the script from the database
CommandReply RedisCluster::get_script(const std::string& key)
{
    std::string prefixed_str = "{" + this->_db_nodes[0].prefix + "}." + key;

    Command cmd;
    cmd.add_field("AI.SCRIPTGET");
    cmd.add_field(prefixed_str, true);
    cmd.add_field("SOURCE");
    return this->run(cmd);
}

// Connect to the cluster at the address and port
inline void RedisCluster::_connect(std::string address_port)
{
    int n_trials = 10;
    for (int i = 1; i <= n_trials; i++) {
        try {
            this->_redis_cluster = new sw::redis::RedisCluster(address_port);
            return;
        }
        catch (std::exception& e) {
            if (this->_redis_cluster != NULL) {
                delete this->_redis_cluster;
                this->_redis_cluster = NULL;
            }
            if (i == n_trials) {
                throw std::runtime_error(e.what());
            }
        }
        catch (...) {
            if (this->_redis_cluster != NULL) {
                delete this->_redis_cluster;
                this->_redis_cluster = NULL;
            }
            if (i == n_trials) {
                throw std::runtime_error("A non-standard exception was "\
                                         "encountered during client "\
                                         "connection.");
            }
        }

        // Sleep before the next atttenpt
        if (this->_redis_cluster != NULL) {
            delete this->_redis_cluster;
            this->_redis_cluster = NULL;
        }
        std::this_thread::sleep_for(std::chrono::seconds(2));
    }
}

// Map the RedisCluster via the CLUSTER SLOTS command
inline void RedisCluster::_map_cluster()
{
    // Clear out our old map
    this->_db_nodes.clear();
    this->_address_node_map.clear();

    // Build the CLUSTER SLOTS command
    Command cmd;
    cmd.add_field("CLUSTER");
    cmd.add_field("SLOTS");

    // Run it
    CommandReply reply(this->_redis_cluster->
                 command(cmd.begin(), cmd.end()));
    if (reply.has_error() > 0) {
        throw std::runtime_error("CLUSTER SLOTS command failed");
    }

    // Process results
    this->_parse_reply_for_slots(reply);
}

// Get the prefix that can be used to address the correct database
// for a given command
std::string RedisCluster::_get_db_node_prefix(Command& cmd)
{
    // Extract the keys from the command
    std::vector<std::string> keys = cmd.get_keys();
    if (keys.size() == 0) {
        throw std::runtime_error("Command " + cmd.to_string() +
                                 " does not have a key value.");
    }

    // Walk through the keys to find the prefix
    std::string prefix;
    std::vector<std::string>::iterator key_it = keys.begin();
    for ( ; key_it != keys.end(); key_it++) {
        uint16_t hash_slot = this->_get_hash_slot(*key_it);
        uint16_t db_index = this->_get_dbnode_index(hash_slot, 0,
                                           this->_db_nodes.size()-1);
        if (prefix.size() == 0) {
            prefix = this->_db_nodes[db_index].prefix;
        }
        else if (prefix != this->_db_nodes[db_index].prefix) {
            throw std::runtime_error("Multi-key commands are "\
                                        "not valid: " +
                                        cmd.to_string());
        }
    }

    // Done
    return prefix;
}

// Process the CommandReply for CLUSTER SLOTS to build DBNode information
inline void RedisCluster::_parse_reply_for_slots(CommandReply& reply)
{
    /* Each reply element of the main message, of which there should
    be n_db_nodes, is:
    0) (integer) min slot
    1) (integer) max slot
    2) 0) "ip address"
       1) (integer) port
       2) "name"
    */
    size_t n_db_nodes = reply.n_elements();
    this->_db_nodes = std::vector<DBNode>(n_db_nodes);

    for (int i=0; i<n_db_nodes; i++) {
        this->_db_nodes[i].lower_hash_slot = reply[i][0].integer();
        this->_db_nodes[i].upper_hash_slot = reply[i][1].integer();
        this->_db_nodes[i].ip = std::string(reply[i][2][0].str(),
                                            reply[i][2][0].str_len());
        this->_db_nodes[i].port = reply[i][2][1].integer();
        this->_db_nodes[i].name = std::string(reply[i][2][2].str(),
                                              reply[i][2][2].str_len());
        bool acceptable_prefix = false;
        int n_hashes = this->_db_nodes[i].upper_hash_slot -
                       this->_db_nodes[i].lower_hash_slot + 1;
        int k = 0;
        for (k = 0; !acceptable_prefix && k <= n_hashes; k++) {
            this->_db_nodes[i].prefix = this->_get_crc16_prefix(
                                        this->_db_nodes[i].lower_hash_slot+k);
            std::string prefix = this->_db_nodes[i].prefix;
            bool found_bracket = false;
            for (int j = 0; j < prefix.size(); j++) {
                if (prefix[j] == '}') {
                    found_bracket = true;
                    break;
                }
            }
            if (!found_bracket)
                acceptable_prefix = true;
        }

        if (k > n_hashes)
            throw std::runtime_error("A prefix could not be generated "\
                                     "for this cluster config.");

        this->_address_node_map.insert({this->_db_nodes[i].ip + ":"
                                    + std::to_string(this->_db_nodes[i].port),
                                    &this->_db_nodes[i]});
    }

    //Put the vector of db nodes in order based on lower hash slot
    std::sort(this->_db_nodes.begin(), this->_db_nodes.end());
}

// Get a DBNode prefix for the provided hash slot
std::string RedisCluster::_get_crc16_prefix(uint64_t hash_slot)
{
    uint64_t byte_filter = {255};
    uint64_t crc_out = this->_crc16_inverse(hash_slot);
    crc_out = crc_out >> 16;

    // Get the two character prefix
    char prefix[2] = {0};
    for (int i = 1; i >= 0; i--) {
        prefix[i] = (crc_out & byte_filter);
        crc_out = crc_out >> 8;
    }
    std::string prefix_str = std::string(prefix, 2);
    return prefix_str;
}

// Perform an inverse CRC16 calculation
uint64_t RedisCluster::_crc16_inverse(uint64_t remainder)
{
    uint64_t digit = 1;
    uint64_t poly = 69665; //x^16 + x^12 + x^5 + 1

    for (int i = 0; i < 16; i++) {
        if ((remainder & digit) != 0)
        remainder ^= poly;
        digit = digit << 1;
        poly = poly << 1;
    }
    return remainder;
}

// Determine if the key has a substring enclosed by "{" and "}" characters
bool RedisCluster::_has_hash_tag(const std::string& key)
{
    size_t first = key.find('{');
    size_t second = key.find('}');
    return (first != std::string::npos && second != std::string::npos &&
            second > first);
}

// Return the key enclosed by "{" and "}" characters
std::string RedisCluster::_get_hash_tag(const std::string& key)
{
    // If no hash tag, bail
    if (!this->_has_hash_tag(key))
        return key;

    // Extract the hash tag
    size_t first = key.find('{');
    size_t second = key.find('}');
    return key.substr(first + 1, second - first - 1);
}

// Get the hash slot for a key
uint16_t RedisCluster::_get_hash_slot(const std::string& key)
{
    std::string hash_key = this->_get_hash_tag(key);
    return sw::redis::crc16(hash_key.c_str(), hash_key.size()) % 16384;
}

// Get the index of the DBNode responsible for the hash slot
uint16_t RedisCluster::_get_dbnode_index(uint16_t hash_slot,
                                   unsigned lhs, unsigned rhs)
{
    // Find the DBNode via binary search
    uint16_t m = (lhs + rhs) / 2;

    // If this is the correct slot, we're done
    if (this->_db_nodes[m].lower_hash_slot <= hash_slot &&
        this->_db_nodes[m].upper_hash_slot >= hash_slot) {
        return m;
    }

    // Otherwise search in the appropriate half
    else {
        if (this->_db_nodes[m].lower_hash_slot > hash_slot)
            return this->_get_dbnode_index(hash_slot, lhs, m - 1);
        else
            return this->_get_dbnode_index(hash_slot, m + 1, rhs);
    }
}

// Attaches a prefix and constant suffix to keys to enforce identical
//  hash slot constraint
std::vector<std::string>
RedisCluster::_get_tmp_names(std::vector<std::string> names,
                             std::string db_prefix)
{
    std::vector<std::string> tmp;
    std::vector<std::string>::iterator it = names.begin();
    for ( ; it != names.end(); it++) {
        std::string new_key = "{" + db_prefix + "}." + *it + ".TMP";
        tmp.push_back(new_key);
    }
    return tmp;
}

// Delete multiple keys (assumesthat all keys use the same hash slot)
void RedisCluster::_delete_keys(std::vector<std::string> keys)
{
    // Build the command
    Command cmd;
    cmd.add_field("DEL");
    cmd.add_fields(keys, true);

    // Run it, ignoring failure
    (void)this->run(cmd);
}

// Run a model in the database that uses dagrun
void RedisCluster::__run_model_dagrun(const std::string& key,
                                      std::vector<std::string> inputs,
                                      std::vector<std::string> outputs)
{
    /* This function will run a RedisAI model.  Because the RedisAI
    AI.RUNMODEL and AI.DAGRUN commands assume that the tensors
    and model are all on the same node.  As a result, we will
    have to retrieve all input tensors that are not on the same
    node as the model and set temporary
    */

    // TODO We need to make sure that no other clients are using the
    // same keys and model because we may end up overwriting or having
    // race conditions on who can use the model, etc.

    DBNode* db = this->_get_model_script_db(key, inputs, outputs);

    // Create list of input tensors that do not hash to db slots
    std::unordered_set<std::string> remote_inputs;
    for (int i = 0; i < inputs.size(); i++) {
        uint16_t hash_slot = this->_get_hash_slot(inputs[i]);
        if (hash_slot < db->lower_hash_slot ||
            hash_slot > db->upper_hash_slot) {
            remote_inputs.insert(inputs[i]);
        }
    }

    // Retrieve tensors that do not hash to db,
    // rename the tensors to {prefix}.tensor_name.TMP
    // TODO we need to make sure users don't use the .TMP suffix
    // or check that the key does not exist
    for (int i = 0; i < inputs.size(); i++) {
        if (remote_inputs.count(inputs[i]) > 0) {
            std::string new_key = "{" + db->prefix + "}." + inputs[i] + ".TMP";
            this->copy_tensor(inputs[i], new_key);
            remote_inputs.erase(inputs[i]);
            remote_inputs.insert(new_key);
            inputs[i] = new_key;
        }
    }

    // Create a renaming scheme for output tensor
    std::unordered_map<std::string, std::string> remote_outputs;
    for (int i = 0; i < outputs.size(); i++) {
        uint16_t hash_slot = this->_get_hash_slot(outputs[i]);
        if (hash_slot < db->lower_hash_slot ||
            hash_slot > db->upper_hash_slot) {
            std::string tmp_name = "{" + db->prefix + "}." +
                                outputs[i] + ".TMP";
            remote_outputs.insert({outputs[i], tmp_name});
            outputs[i] = remote_outputs[outputs[i]];
        }
    }

    // Build the DAGRUN command
    std::string model_name = "{" + db->prefix + "}." + key;
    Command cmd;
    cmd.add_field("AI.DAGRUN");
    cmd.add_field("LOAD");
    cmd.add_field(std::to_string(inputs.size()));
    cmd.add_fields(inputs);
    cmd.add_field("PERSIST");
    cmd.add_field(std::to_string(outputs.size()));
    cmd.add_fields(outputs);
    cmd.add_field("|>");
    cmd.add_field("AI.MODELRUN");
    cmd.add_field(model_name, true);
    cmd.add_field("INPUTS");
    cmd.add_fields(inputs);
    cmd.add_field("OUTPUTS");
    cmd.add_fields(outputs);

    // Run it
    CommandReply reply = this->run(cmd);
    if (reply.has_error()) {
        throw std::runtime_error("Failed to execute DAGRUN");
    }

    // Delete temporary input tensors
    std::unordered_set<std::string>::const_iterator i_it =
        remote_inputs.begin();
    for ( ; i_it !=  remote_inputs.end(); i_it++)
        this->delete_tensor(*i_it);

    // Move temporary output to the correct location and
    // delete temporary output tensors
    std::unordered_map<std::string, std::string>::const_iterator j_it =
        remote_outputs.begin();
    for ( ; j_it != remote_outputs.end(); j_it++)
        this->rename_tensor(j_it->second, j_it->first);
}

// Retrieve the optimum model prefix for the set of inputs
DBNode* RedisCluster::_get_model_script_db(const std::string& name,
                                           std::vector<std::string>& inputs,
                                           std::vector<std::string>& outputs)
{
    /* This function calculates the optimal model name to use
    to run the provided inputs.  If a cluster is not being used,
    the model name is returned, else a prefixed model name is returned.
    */

    // TODO we should randomly choose the max if there are multiple maxes

    std::vector<int> hash_slot_tally(this->_db_nodes.size(), 0);

    for (int i = 0; i < inputs.size(); i++) {
        uint16_t hash_slot = this->_get_hash_slot(inputs[i]);
        uint16_t db_index = this->_get_dbnode_index(hash_slot, 0,
                                                    this->_db_nodes.size());
        hash_slot_tally[db_index]++;
    }

    for (int i = 0; i < outputs.size(); i++) {
        uint16_t hash_slot = this->_get_hash_slot(outputs[i]);
        uint16_t db_index = this->_get_dbnode_index(hash_slot, 0,
                                                    this->_db_nodes.size());
        hash_slot_tally[db_index]++;
    }

    // Determine which DBNode has the most hashes
    int max_hash = -1;
    DBNode* db = NULL;
    for (int i = 0; i < this->_db_nodes.size(); i++) {
        if (hash_slot_tally[i] > max_hash) {
            max_hash = hash_slot_tally[i];
            db = &(this->_db_nodes[i]);
        }
    }
    return db;
}

// EOF
