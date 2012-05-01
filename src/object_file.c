#include <stdlib.h>
#include <fcntl.h>
#include <inttypes.h>
#include <unistd.h>
#include <sys/stat.h>
#include <sys/mman.h>
#include <math.h>

#include "dbg.h"
#include "mem.h"
#include "bstring.h"
#include "database.h"
#include "block.h"
#include "object_file.h"

//==============================================================================
//
// Functions
//
//==============================================================================

//======================================
// Utility
//======================================

// Checks if a file exists.
//
// path - The path of the file.
//
// Returns true if the file exists. Otherwise returns false.
bool file_exists(bstring path)
{
    struct stat buffer;
    int rc = stat(bdata(path), &buffer);
    return (rc == 0);
}


//======================================
// Block Sorting
//======================================

// Compares two blocks and sorts them based on starting min object identifier
// and then by id.
int compare_block_info(const void *_a, const void *_b)
{
    sky_block_info **a = (sky_block_info **)_a;
    sky_block_info **b = (sky_block_info **)_b;

    // Sort by min object id first.
    if((*a)->min_object_id > (*b)->min_object_id) {
        return 1;
    }
    else if((*a)->min_object_id < (*b)->min_object_id) {
        return -1;
    }
    else {
        // If min object ids are the same then sort by min timestamp.
        if((*a)->min_timestamp > (*b)->min_timestamp) {
            return 1;
        }
        else if((*a)->min_timestamp < (*b)->min_timestamp) {
            return -1;
        }
        else {
            // If min timestamps are the same then sort by block id.
            if((*a)->id > (*b)->id) {
                return 1;
            }
            else if((*a)->id < (*b)->id) {
                return -1;
            }
            else {
                return 0;
            }
        }
    }
}

// Compares two blocks and sorts them by id. This is used when serializing block
// info to the header file.
int compare_block_info_by_id(const void *_a, const void *_b)
{
    sky_block_info **a = (sky_block_info **)_a;
    sky_block_info **b = (sky_block_info **)_b;

    if((*a)->id > (*b)->id) {
        return 1;
    }
    else if((*a)->id < (*b)->id) {
        return -1;
    }
    else {
        return 0;
    }
}

// Sorts blocks by object id and block id.
//
// infos - The array of block info objects.
// count - The number of blocks.
void sort_blocks(sky_block_info **infos, uint32_t count)
{
    qsort(infos, count, sizeof(sky_block_info*), compare_block_info);
}



//======================================
// Header Management
//======================================

// Retrieves the file path of an object file's header file.
//
// object_file - The object file who owns the header file.
bstring get_header_file_path(sky_object_file *object_file)
{
    return bformat("%s/header", bdata(object_file->path)); 
}

// Saves header information to file.
//
// object_file - The object file containing the header information.
//
// Returns 0 if successful, otherwise returns -1.
int save_header(sky_object_file *object_file)
{
    int rc;

    // Copy infos to a new array to re-sort.
    uint32_t block_count = object_file->block_count;
    sky_block_info **infos = malloc(sizeof(sky_block_info*) * block_count); check_mem(infos);
    memcpy(infos, object_file->infos, sizeof(sky_block_info*) * block_count);
    qsort(infos, block_count, sizeof(sky_block_info*), compare_block_info_by_id);

    // Open the header file.
    bstring path = get_header_file_path(object_file); check_mem(path);
    FILE *file = fopen(bdata(path), "w");
    check(file, "Failed to open header file for writing: %s",  bdata(path));
    
    // Write database format version & block size.
    rc = fwrite(&object_file->version, sizeof(object_file->version), 1, file);
    check(rc == 1, "Unable to write version");
    rc = fwrite(&object_file->block_size, sizeof(object_file->block_size), 1, file);
    check(rc == 1, "Unable to write block size");

    // Write block count.
    rc = fwrite(&block_count, sizeof(block_count), 1, file);
    check(rc == 1, "Unable to write block count");

    // Read block info items until end of file.
    uint32_t i;
    for(i=0; i<block_count; i++) {
        sky_block_info *info = infos[i];

        // Write object id range.
        rc = fwrite(&info->min_object_id, sizeof(info->min_object_id), 1, file);
        check(rc == 1, "Unable to write min object id : blk%d", i);
        rc = fwrite(&info->max_object_id, sizeof(info->max_object_id), 1, file);
        check(rc == 1, "Unable to read max object id : blk%d", i);
        
        // Read timestamp range.
        rc = fwrite(&info->min_timestamp, sizeof(info->min_timestamp), 1, file);
        check(rc == 1, "Unable to read min timestamp : blk%d", i);
        rc = fwrite(&info->max_timestamp, sizeof(info->max_timestamp), 1, file);
        check(rc == 1, "Unable to read max timestamp : blk%d", i);
    }

    // Close the file.
    fclose(file);

    // Clean up.
    bdestroy(path);
    free(infos);

    return 0;
    
error:
    bdestroy(path);
    if(file) fclose(file);
    if(infos) free(infos);
    return -1;
}

// Loads header information from file.
//
// object_file - The object file where the header is stored.
//
// Returns 0 if successful, otherwise returns -1.
int load_header(sky_object_file *object_file)
{
    FILE *file;
    sky_block_info **infos = NULL;
    uint32_t version = 1;
    uint32_t block_count = 0;

    // Retrieve file stats on header file
    bstring path = get_header_file_path(object_file); check_mem(path);
    
    // Read in header file if it exists.
    if(file_exists(path)) {
        file = fopen(bdata(path), "r");
        check(file, "Failed to open header file for reading: %s",  bdata(path));

        // Read database format version & block size.
        check(fread(&version, sizeof(version), 1, file) == 1, "Unable to read version");
        check(fread(&object_file->block_size, sizeof(object_file->block_size), 1, file) == 1, "Unable to read block size");

        // Read block count.
        check(fread(&block_count, sizeof(block_count), 1, file) == 1, "Unable to read block count");
        infos = malloc(sizeof(sky_block_info*) * block_count);
        if(block_count > 0) check_mem(infos);

        // Read block info items until end of file.
        uint32_t i;
        for(i=0; i<block_count && !feof(file); i++) {
            // Allocate info.
            sky_block_info *info = malloc(sizeof(sky_block_info));
            
            // Set index.
            info->id = i;
            info->spanned = false;
            
            // Read object id range.
            check(fread(&info->min_object_id, sizeof(info->min_object_id), 1, file) == 1, "Unable to read min object id : block #%d", i);
            check(fread(&info->max_object_id, sizeof(info->max_object_id), 1, file) == 1, "Unable to read max object id : block #%d", i);
            
            // Read timestamp range.
            check(fread(&info->min_timestamp, sizeof(info->min_timestamp), 1, file) == 1, "Unable to read min timestamp : block #%d", i);
            check(fread(&info->max_timestamp, sizeof(info->max_timestamp), 1, file) == 1, "Unable to read max timestamp : block #%d", i);
            
            infos[i] = info;
        }

        // Close the file.
        fclose(file);

        // Sort ranges by starting object id.
        sort_blocks(infos, block_count);
        
        // Determine spanned blocks.
        sky_object_id_t last_object_id = -1;
        for(i=0; i<block_count; i++) {
            // If this is a single object block then track the object id to
            // possibly mark it as spanned.
            if(infos[i]->min_object_id == infos[i]->max_object_id && infos[i]->min_object_id > 0) {
                // If it has spanned since the last block then mark it and the
                // previous block.
                if(infos[i]->min_object_id == last_object_id) {
                    infos[i]->spanned = true;
                    infos[i-1]->spanned = true;
                }
                // If this is the first block with one object then store the id.
                else {
                    last_object_id = infos[i]->min_object_id;
                }
            }
            // Clear out last object id for multi-object blocks.
            else {
                last_object_id = -1;
            }
        }
    }

    // Store version and block information on object file.
    object_file->version = version;
    object_file->block_count = block_count;
    object_file->infos = infos;

    // Clean up.
    bdestroy(path);

    return 0;

error:
    if(file) fclose(file);
    bdestroy(path);
    return -1;
}

// Unloads the header data.
//
// object_file - The object file where the header data is stored.
//
// Returns 0 if successful, otherwise returns -1.
int unload_header(sky_object_file *object_file)
{
    if(object_file) {
        // Free block infos.
        uint32_t i;
        if(object_file->infos) {
            for(i=0; i<object_file->block_count; i++) {
                free(object_file->infos[i]);
                object_file->infos[i] = NULL;
            }

            free(object_file->infos);
            object_file->infos = NULL;
        }

        object_file->version = 0;
        object_file->block_size = SKY_DEFAULT_BLOCK_SIZE;
        object_file->block_count = 0;
    }
    
    return 0;
}



//======================================
// Action Management
//======================================

// Loads action information from file.
//
// object_file - The object file where the action information is stored.
//
// Returns 0 if successful, otherwise returns -1.
int load_actions(sky_object_file *object_file)
{
    FILE *file;
    sky_action **actions = NULL;
    char *buffer;
    sky_object_file_action_count_t count = 0;
    
    // Retrieve file stats on actions file
    bstring path = bformat("%s/actions", bdata(object_file->path)); check_mem(path);
    
    // Read in actions file if it exists.
    if(file_exists(path)) {
        file = fopen(bdata(path), "r");
        check(file, "Failed to open action file: %s",  bdata(path));
        
        // Read action count.
        fread(&count, sizeof(count), 1, file);
        actions = malloc(sizeof(sky_action*) * count);
        if(count > 0) check_mem(actions);
        
        // Read actions until end of file.
        uint32_t i;
        uint16_t length;
        for(i=0; i<count && !feof(file); i++) {
            sky_action *action = malloc(sizeof(sky_action)); check_mem(action);
            
            // Read action id and name length.
            check(fread(&action->id, sizeof(int32_t), 1, file) == 1, "Corrupt actions file");
            check(fread(&length, sizeof(length), 1, file) == 1, "Corrupt actions file");

            // Read action name.
            buffer = calloc(1, length+1); check_mem(buffer);
            check(fread(buffer, length, 1, file) == 1, "Corrupt actions file");
            action->name = bfromcstr(buffer); check_mem(action->name);
            free(buffer);
            
            // Append to array.
            actions[i] = action;
        }
        
        // Close the file.
        fclose(file);
    }

    // Store action list on object file.
    object_file->actions = actions;
    object_file->action_count = count;
    
    // Clean up.
    bdestroy(path);
    
    return 0;

error:
    if(file) fclose(file);
    if(buffer) free(buffer);
    bdestroy(path);
    return -1;
}

// Unloads the action data.
//
// object_file - The object file where the action data is stored.
//
// Returns 0 if successful, otherwise returns -1.
int unload_actions(sky_object_file *object_file)
{
    if(object_file) {
        // Release actions.
        if(object_file->actions) {
            uint32_t i=0;
            for(i=0; i<object_file->action_count; i++) {
                bdestroy(object_file->actions[i]->name);
                free(object_file->actions[i]);
                object_file->actions[i] = NULL;
            }
            free(object_file->actions);
            object_file->actions = NULL;
        }
        
        object_file->action_count = 0;
    }
    
    return 0;
}


//======================================
// Property Management
//======================================

// Loads property information from file.
//
// object_file - The object file from which to load property definitions.
//
// Returns 0 if sucessfuly, otherwise returns -1.
int load_properties(sky_object_file *object_file)
{
    FILE *file;
    sky_property **properties = NULL;
    char *buffer;
    sky_object_file_property_count_t count = 0;
    
    // Retrieve file stats on properties file
    bstring path = bformat("%s/properties", bdata(object_file->path)); check_mem(path);
    
    // Read in properties file if it exists.
    if(file_exists(path)) {
        file = fopen(bdata(path), "r");
        check(file, "Failed to open properties file: %s",  bdata(path));
        
        // Read properties count.
        fread(&count, sizeof(count), 1, file);
        properties = malloc(sizeof(sky_property*) * count);
        
        // Read properties until end of file.
        uint32_t i;
        uint16_t length;
        for(i=0; i<count && !feof(file); i++) {
            sky_property *property = malloc(sizeof(sky_property)); check_mem(property);
            
            // Read property id and name length.
            check(fread(&property->id, sizeof(int16_t), 1, file) == 1, "Corrupt properties file");
            check(fread(&length, sizeof(length), 1, file) == 1, "Corrupt properties file");

            // Read property name.
            buffer = calloc(1, length+1); check_mem(buffer);
            check(fread(buffer, length, 1, file) == 1, "Corrupt properties file");
            property->name = bfromcstr(buffer); check_mem(property->name);
            free(buffer);
            
            // Append property to array.
            properties[i] = property;
        }
        
        // Close the file.
        fclose(file);
    }

    // Store property list on object file.
    object_file->properties = properties;
    object_file->property_count = count;
    
    // Clean up.
    bdestroy(path);
    
    return 0;

error:
    if(file) fclose(file);
    if(buffer) free(buffer);
    bdestroy(path);
    return -1;
}

// Unloads the property data.
//
// object_file - The object file where the property data is stored.
//
// Returns 0 if successful, otherwise returns -1.
int unload_properties(sky_object_file *object_file)
{
    if(object_file) {
        // Release properties.
        if(object_file->properties) {
            uint32_t i=0;
            for(i=0; i<object_file->property_count; i++) {
                bdestroy(object_file->properties[i]->name);
                free(object_file->properties[i]);
                object_file->properties[i] = NULL;
            }
            free(object_file->properties);
            object_file->properties = NULL;
        }
        
        object_file->property_count = 0;
    }
    
    return 0;
}


//======================================
// Block Management
//======================================

// Retrieves the file path of an object file's data file.
//
// object_file - The object file who owns the data file.
bstring get_data_file_path(sky_object_file *object_file)
{
    return bformat("%s/data", bdata(object_file->path)); 
}


// Calculates the byte offset in a data file for a block. This is simply the
// block index multiplied by the block size.
//
// object_file - The object file that contains the block.
// info        - The header info about the block.
//
// Returns the number of bytes from the start of the data file where the block
// begins.
ptrdiff_t get_block_offset(sky_object_file *object_file, sky_block_info *info)
{
    return (object_file->block_size * info->id);
}

// Unmaps a data file for an object.
//
// object_file - The object file that owns the data file.
//
// Returns 0 if successful, otherwise returns -1.
int unmap_data_file(sky_object_file *object_file)
{
    // Unmap file.
    if(object_file->data != NULL) {
        munmap(object_file->data, object_file->data_length);
        object_file->data = NULL;
    }
    
    // Close file descriptor.
    if(object_file->data_fd != 0) {
        close(object_file->data_fd);
        object_file->data_fd = 0;
    }
    
    object_file->data_length = 0;
    
    return 0;
}

// Maps the data file for an object file. If the data file is already mapped
// then it is remapped.
//
// object_file - The object file that owns the data file.
//
// Returns 0 if successful, otherwise returns -1.
int map_data_file(sky_object_file *object_file)
{
    int rc;
    void *ptr;
    
    // Calculate the data length.
    size_t data_length;
    if(object_file->block_count == 0) {
        data_length = object_file->block_size;
    }
    else {
        data_length = object_file->block_count * object_file->block_size;
    }
    
    // Close mapping if remapping isn't supported.
    if(!MREMAP_AVAILABLE) {
        unmap_data_file(object_file);
    }
    
    // Find the path to the data file.
    bstring path = get_data_file_path(object_file); check_mem(path);

    // Open the data file and map it if it is not currently open.
    if(object_file->data_fd == 0) {
        object_file->data_fd = open(bdata(path), O_RDWR | O_CREAT, S_IRUSR | S_IWUSR);
        check(object_file->data_fd != -1, "Failed to open data file descriptor: %s",  bdata(path));

        // Truncate the file to the appropriate size (larger or smaller).
        rc = ftruncate(object_file->data_fd, data_length);
        check(rc == 0, "Unable to truncate data file");
        
        // Memory map the data file.
        ptr = mmap(0, data_length, PROT_READ | PROT_WRITE, MAP_SHARED, object_file->data_fd, 0);
        check(ptr != MAP_FAILED, "Unable to memory map data file");
    }
    // If we already have the data mapped then simply remap it to the
    // appropriate size.
    else {
#if MREMAP_AVAILABLE
        ptr = mremap(object_file->data, object_file->data_length, data_length, MREMAP_MAYMOVE);
        check(ptr != MAP_FAILED, "Unable to remap data file");
#endif
    }

    // Update the object file.
    object_file->data = ptr;
    object_file->data_length = data_length;

    bdestroy(path);
    
    return 0;

error:
    object_file->data = NULL;
    bdestroy(path);
    return -1;
}

// Serializes a block in memory and writes it to disk.
// 
// block       - The in-memory block to write to disk.
//
// Returns 0 if successful. Otherwise returns -1.
int save_block(sky_block *block)
{
    int rc;

    sky_object_file *object_file = block->object_file;
    sky_block_info *info = block->info;

    // Move pointer to starting position of block.
    ptrdiff_t offset = get_block_offset(object_file, info);
    void *addr = object_file->data + offset;

    // Serialize block.
    ptrdiff_t ptrdiff;
    rc = sky_block_serialize(block, addr, &ptrdiff);
    check(rc == 0, "Failed to serialize block: %d", info->id);

    // Update block info.
    sky_block_update_info(block);

    return 0;

error:
    return -1;
}

// Creates an empty block in the object file and returns it.
//
// object_file - The object file that will own the new block.
// ret         - The block object returned to the caller.
//
// Returns 0 if successful, otherwise returns -1.
int create_block(sky_object_file *object_file, sky_block **ret)
{
    int rc;
    
    // Increment block count and resize block info memory.
    object_file->block_count++;
    object_file->infos = realloc(object_file->infos, sizeof(sky_block_info*) * object_file->block_count);
    if(object_file->block_count > 0) check_mem(object_file->infos);

    // Create new block.
    sky_block_info *info = malloc(sizeof(sky_block_info)); check_mem(info);
    info->id = object_file->block_count-1;
    info->min_object_id = 0LL;
    info->max_object_id = 0LL;
    info->spanned = false;
    object_file->infos[object_file->block_count-1] = info;

    // Remap data file.
    rc = map_data_file(object_file);
    check(rc == 0, "Unable to remap data file");

    // Save empty block to file.
    sky_block *block = sky_block_create(object_file, info);
    rc = save_block(block);
    check(rc == 0, "Unable to initialize new block");

    // Sort blocks.
    sort_blocks(object_file->infos, object_file->block_count);

    // Return the block.
    *ret = block;

    return 0;
    
error:
    if(block) sky_block_free(block);
    *ret = block = NULL;

    return -1;
}

// Finds the correct block to add an event to.
//
// Insertion has several rules to ensure consistency. There are two types of
// blocks: single object blocks and multi-object blocks. Single object blocks
// are spans of two or more blocks that have a single object. Multi-object
// blocks are single blocks that have multiple object ids. An object that is
// stored in a single block (i.e. doesn't span blocks) is considered a
// multi-object block since there is probably room to add more objects.
// 
// 1. An object id range is unique to a block. If a block has object ids 5 - 8
//    then no other block can overlap into those ids.
//
// 2. Only multi-object blocks can have object ids outside its range added.
//
// 3. Objects are added to the first block where the object id is included in
//    the object id range or is less than the minimum object id of the block.
//
// 4. If no block is found by the end then the last block is used for insertion.
//
// object_file - The object file that the event is being added to.
// event       - The event to add to the object file.
// ret         - A reference to the correct block info returned to the caller.
//
// Returns 0 if successfully finds a block. Otherwise returns -1.
int find_insertion_block(sky_object_file *object_file, sky_event *event, sky_block_info **ret)
{
    int i, n, rc;
    sky_block_info *info = NULL;
    info = NULL;
    
    // Initialize return value to NULL.
    *ret = NULL;
    
    // Extract object id and timestamp from event.
    sky_object_id_t object_id = event->object_id;
    sky_timestamp_t timestamp = event->timestamp;

    // Loop over sorted blocks to find the appropriate insertion point.
    n = object_file->block_count;
    for(i=0; i<n; i++) {
        info = object_file->infos[i];
        
        // If block is within range then use the block.
        if(object_id >= info->min_object_id && object_id <= info->max_object_id) {
            // If this is a single object block then find the appropriate block
            // based on timestamp.
            if(info->spanned) {
                // Find first block where timestamp is before the max.
                while(i<n && object_file->infos[i]->min_object_id == object_id) {
                    if(timestamp <= object_file->infos[i]->max_timestamp) {
                        *ret = info;
                        break;
                    }
                    i++;
                }
                
                // If this event is being appended to the object path then use
                // the last block.
                if(*ret == NULL) {
                    *ret = object_file->infos[i-1];
                }

                break;
            }
            // If this is a multi-object block then simply use it.
            else {
                *ret = info;
                break;
            }
        }
        // If block is before this object id range, then use the block if it
        // is a multi-object block.
        else if(object_id < info->min_object_id && !info->spanned) {
            *ret = info;
            break;
        }
    }
    
    // If we haven't found a block then it means that the object id is after all
    // other object ids or that we are inserting before a single object block or
    // that we have no blocks.
    if(*ret == NULL) {
        // Find the last block if one exists.
        sky_block_info *last_info = (object_file->block_count > 0 ? object_file->infos[object_file->block_count-1] : NULL);
        
        // If the last block available is unspanned then use it.
        if(last_info != NULL && !last_info->spanned) {
            *ret = last_info;
        }
        // Otherwise just create a new block.
        else {
            sky_block *block;
            rc = create_block(object_file, &block);
            check(rc == 0, "Unable to create block");
            *ret = block->info;
            sky_block_free(block);
        }
    }
    
    return 0;

error:
    return -1;
}

// Reads a block in from disk and deserializes it.
// 
// object_file - The object file that contains the block.
// info        - A reference to the block position.
// ret         - The reference to the block returned to the caller.
//
// Returns 0 if successful. Otherwise returns -1.
int load_block(sky_object_file *object_file, sky_block_info *info, sky_block **ret)
{
    int rc;
    
    // Create pointer at starting position of block.
    ptrdiff_t offset = get_block_offset(object_file, info);
    void *addr = object_file->data + offset;

    // Deserialize block.
    ptrdiff_t ptrdiff;
    sky_block *block = sky_block_create(object_file, info);
    rc = sky_block_deserialize(block, addr, &ptrdiff);
    check(rc == 0, "Failed to deserialize block: %d", info->id);
    
    // Assign block to return value.
    *ret = block;
    
    return 0;

error:
    return -1;
}

// Creates a target block and appends it to a list of blocks.
//
// source - The block that is the source of paths/events for the target block.
// target - The target block that will be returned.
// blocks - An array of blocks.
// count  - The number of blocks in the array.
//
// Returns 0 if successful, otherwise returns -1.
int create_target_block(sky_block *source, sky_block **target, sky_block ***blocks, int *count)
{
    int rc;
    
    // Create new block.
    rc = create_block(source->object_file, target);
    check(rc == 0, "Unable to create target block");
    
    // Increment block count.
    (*count)++;
    *blocks = realloc(*blocks, sizeof(sky_block*) * (*count));
    check_mem(*blocks);
    
    // Append target block.
    (*blocks)[*count-1] = *target;

    return 0;

error:
    if(*target) sky_block_free(*target);
    *target = NULL;

    return -1;    
}

// Attempts to split a given block into multiple smaller blocks if the block
// exceeds the maximum block size allowed for an object file.
//
// block           - The block to attempt to split.
// affected_blocks - An array of blocks. This includes the original block passed
//                   in as well as any blocks split off the original.
// affected_block_count - The number of blocks in the affected_blocks array.
//
// Returns 0 if successful, otherwise returns -1.
int split_block(sky_block *block, sky_block ***affected_blocks, int *affected_block_count)
{
    int rc;
    sky_block *target_block;
    sky_event **events;
    uint32_t event_count;

    // Add original block to list of affected blocks.
    *affected_block_count = 1;
    *affected_blocks = malloc(sizeof(sky_block*)); check_mem(*affected_blocks);
    (*affected_blocks)[0] = block;

    // If block size has not been exceeded then exit this function immediately.
    uint32_t block_serialized_length = sky_block_get_serialized_length(block);
    if(block_serialized_length <= block->object_file->block_size) {
        return 0;
    }
    
    // Extract paths and info from original block.
    bool spanned = block->info->spanned;
    sky_path **paths = block->paths;
    sky_path_count_t path_count = block->path_count;
    block->paths = NULL;
    block->path_count = 0;
    
    // Assign original block as first target block.
    target_block = block;
    
    // Calculate target block size if we were to spread paths evenly across blocks.
    uint32_t max_block_size = block->object_file->block_size - BLOCK_HEADER_LENGTH;
    uint32_t target_block_count = (uint32_t)ceil((double)block_serialized_length / (double)max_block_size);
    uint32_t target_block_size = block_serialized_length / target_block_count;
    
    // Loop over paths and spread them across blocks.
    uint32_t i, j;
    for(i=0; i<path_count; i++) {
        sky_path *path = paths[i];
        uint32_t path_serialized_length = sky_path_get_serialized_length(path);
        
        // If path is already spanned or the path is larger than max block size
        // then spread its events across multiple blocks.
        if(spanned || path_serialized_length > max_block_size) {
            // Extract events from path.
            events = path->events;
            event_count = path->event_count;
            path->events = NULL;
            path->event_count = 0;

            // Mark target block as spanned.
            target_block->info->spanned = true;

            // Split path into spanned blocks.
            sky_path *target_path = NULL;
            for(j=0; j<event_count; j++) {
                sky_event *event = events[j];
                uint32_t event_serialized_length = sky_event_get_serialized_length(event);
                
                // Create new target path if adding event will make path exceed block size.
                if(target_path != NULL) {
                    if(sky_path_get_serialized_length(target_path) + event_serialized_length > max_block_size) {
                        rc = sky_block_add_path(target_block, target_path);
                        check(rc == 0, "Unable to add path to block[1]: %d", target_block->info->id);
                        target_path = NULL;

                        rc = create_target_block(block, &target_block, affected_blocks, affected_block_count);
                        check(rc == 0, "Unable to create target block for block split[1]");

                        // Mark new block as spanned.
                        target_block->info->spanned = true;
                    }
                }
                
                // Create a path if we don't have one available.
                if(target_path == NULL) {
                    target_path = sky_path_create(event->object_id);
                    check_mem(target_path);
                }
                
                // Add event to new target path.
                rc = sky_path_add_event(target_path, event);
                check(rc == 0, "Unable to add event to new target path");
                
                // Add target path to new block if we're at the end.
                if(j == event_count-1) {
                    rc = sky_block_add_path(target_block, target_path);
                    check(rc == 0, "Unable to add path to block[2]: %d", target_block->info->id);
                    target_path = NULL;
                }
            }
            
            // Remove path since it's now been split into smaller subpaths.
            sky_path_free(path);
            
            // Clean up event list.
            free(events);
            events = NULL;
            event_count = 0;
        }
        // Otherwise add path to the target block.
        else {
            block_serialized_length = sky_block_get_serialized_length(target_block);

            // If target block will be larger than target block size then create
            // a new block. Only do this if a path exists on the block though.
            if(target_block->path_count > 0 &&
               block_serialized_length + path_serialized_length > target_block_size)
            {
                rc = create_target_block(block, &target_block, affected_blocks, affected_block_count);
                check(rc == 0, "Unable to create target block for block split[2]");
            }
            
            // Add path to target block.
            rc = sky_block_add_path(target_block, path);
            check(rc == 0, "Unable to add path to block[3]: %d", target_block->info->id);
        }
    }

    return 0;

error:
    // Return an empty blockset on error.
    if(*affected_blocks) free(*affected_blocks);
    *affected_blocks = NULL;
    *affected_block_count = 0;
    
    // Clean up paths extracted from block.
    if(paths) {
        for(i=0; i<path_count; i++) {
            sky_path_free(paths[i]);
        }
        free(paths);
    }
    
    // Clean up events if an error occurred during a path split.
    if(events) {
        for(i=0; i<event_count; i++) {
            sky_event_free(events[i]);
        }
        free(events);
    }
    
    return -1;
}



//======================================
// Lifecycle
//======================================


// Creates a reference to an object file.
// 
// database - A reference to the database that the object file belongs to.
// name - The name of the object file.
//
// Returns a reference to the new object file if successful. Otherwise returns
// null.
sky_object_file *sky_object_file_create(sky_database *database, bstring name)
{
    sky_object_file *object_file;
    
    check(database != NULL, "Cannot create object file without a database");
    check(name != NULL, "Cannot create unnamed object file");
    
    object_file = malloc(sizeof(sky_object_file)); check_mem(object_file);
    object_file->state = SKY_OBJECT_FILE_STATE_CLOSED;
    object_file->name = bstrcpy(name); check_mem(object_file->name);
    object_file->path = bformat("%s/%s", bdata(database->path), bdata(object_file->name));
    check_mem(object_file->path);

    object_file->block_size = SKY_DEFAULT_BLOCK_SIZE;  // Default to 64K blocks.
    object_file->infos = NULL;
    object_file->block_count = 0;

    object_file->actions = NULL;
    object_file->action_count = 0;

    object_file->properties = NULL;
    object_file->property_count = 0;

    object_file->data = NULL;
    object_file->data_length = 0;

    return object_file;
    
error:
    sky_object_file_free(object_file);
    return NULL;
}

// Removes an object file reference from memory.
//
// object_file - The object file to free.
void sky_object_file_free(sky_object_file *object_file)
{
    if(object_file) {
        bdestroy(object_file->name);
        bdestroy(object_file->path);
        free(object_file);
    }
}


//======================================
// State
//======================================

// Opens the object file for reading and writing events.
// 
// object_file - The object file to open.
//
// Returns 0 if successful, otherwise returns -1.
int sky_object_file_open(sky_object_file *object_file)
{
    int rc;
    
    // Validate arguments.
    check(object_file != NULL, "Object file required to open");
    check(object_file->state == SKY_OBJECT_FILE_STATE_CLOSED, "Object file must be closed to open")

    // Create directory if it doesn't exist.
    if(!file_exists(object_file->path)) {
        rc = mkdir(bdata(object_file->path), S_IRWXU);
        check(rc == 0, "Unable to create object file directory: %s", bdata(object_file->path));
    }

    // Load header, action and properties data.
    check(load_header(object_file) == 0, "Unable to load header data");
    check(load_actions(object_file) == 0, "Unable to load action data");
    check(load_properties(object_file) == 0, "Unable to load property data");

    // Map the data file.
    rc = map_data_file(object_file);
    check(rc == 0, "Unable to map data file");
    
    // Flag the object file as locked.
    object_file->state = SKY_OBJECT_FILE_STATE_OPEN;

    return 0;

error:
    sky_object_file_close(object_file);
    return -1;
}

// Closes an object file.
//
// object_file - The object file to close.
//
// Returns 0 if successful, otherwise returns -1.
int sky_object_file_close(sky_object_file *object_file)
{
    // Validate arguments.
    check(object_file != NULL, "Object file required to close");

    // Unload header, action and properties data.
    check(unload_header(object_file) == 0, "Unable to unload header data");
    check(unload_actions(object_file) == 0, "Unable to unload action data");
    check(unload_properties(object_file) == 0, "Unable to unload property data");

    // Unmap data file.
    unmap_data_file(object_file);

    // Update state.
    object_file->state = SKY_OBJECT_FILE_STATE_CLOSED;

    return 0;
    
error:
    return -1;
}


//======================================
// Locking
//======================================


// Locks an object file for writing.
// 
// object_file - The object file to lock.
//
// Returns 0 if successful, otherwise returns -1.
int sky_object_file_lock(sky_object_file *object_file)
{
    FILE *file;

    // Validate arguments.
    check(object_file != NULL, "Object file required to lock");
    check(object_file->state == SKY_OBJECT_FILE_STATE_OPEN, "Object file must be open to lock")

    // Construct path to lock.
    bstring path = bformat("%s/%s", bdata(object_file->path), SKY_OBJECT_FILE_LOCK_NAME); check_mem(path);

    // Raise error if object file is already locked.
    check(!file_exists(path), "Cannot obtain lock: %s", bdata(path));

    // Write pid to lock file.
    file = fopen(bdata(path), "w");
    check(file, "Failed to open lock file: %s",  bdata(path));
    check(fprintf(file, "%d", getpid()) > 0, "Error writing lock file: %s",  bdata(path));
    fclose(file);

    // Flag the object file as locked.
    object_file->state = SKY_OBJECT_FILE_STATE_LOCKED;

    // Clean up.
    bdestroy(path);

    return 0;

error:
    if(file) fclose(file);
    bdestroy(path);
    return -1;
}

// Unlocks an object file.
// 
// object_file - The object file to unlock.
//
// Returns 0 if successful, otherwise returns -1.
int sky_object_file_unlock(sky_object_file *object_file)
{
    FILE *file;
    pid_t pid = 0;

    // Validate arguments.
    check(object_file != NULL, "Object file required to unlock");
    check(object_file->state == SKY_OBJECT_FILE_STATE_LOCKED, "Object file must be locked to unlock")

    // Construct path to lock.
    bstring path = bformat("%s/%s", bdata(object_file->path), SKY_OBJECT_FILE_LOCK_NAME); check_mem(path);

    // If file exists, check its PID and then attempt to remove it.
    if(file_exists(path)) {
        // Read PID from lock.
        file = fopen(bdata(path), "r");
        check(file, "Failed to open lock file: %s",  bdata(path));
        check(fscanf(file, "%d", &pid) > 0, "Error reading lock file: %s", bdata(path));
        fclose(file);

        // Make sure we are removing a lock we created.
        check(pid == getpid(), "Cannot remove lock from another process (PID #%d): %s", pid, bdata(path));

        // Remove lock.
        check(unlink(bdata(path)) == 0, "Unable to remove lock: %s", bdata(path));
    }

    // Flag the object file as open.
    object_file->state = SKY_OBJECT_FILE_STATE_OPEN;

    // Clean up.
    bdestroy(path);

    return 0;

error:
    if(file) fclose(file);
    bdestroy(path);
    return -1;
}
    

//======================================
// Block Management
//======================================

// Determines the number of blocks that a given block spans. This function only
// works on the starting block of a span of blocks.
//
// object_file - The object file containing the blocks.
// block_index - The index of the block inside the object file's block list.
// span_count  - A pointer to where the return value should be stored.
//
// Returns 0 if successful, otherwise returns -1.
int sky_object_file_get_block_span_count(sky_object_file *object_file, uint32_t block_index, uint32_t *span_count)
{
    check(object_file != NULL, "Object file required");
    check(block_index < object_file->block_count, "Block index out of range");
    check(span_count != NULL, "Span count address required");
    
    // Loop until the ending block of the span is found.
    uint32_t index = block_index;
    sky_object_id_t object_id = object_file->infos[index]->min_object_id;
    while(true) {
        index++;

        // If we've reached the end of the blocks or if the object id no longer
        // matches the starting object id then break out of the loop.
        if(index > object_file->block_count-1 ||
           object_id != object_file->infos[index]->min_object_id)
        {
            break;
        }
    }

    // Assign count back to caller's provided address.
    *span_count = (index - block_index);
    
    return 0;

error:
    *span_count = 0;
    return -1;
}


//======================================
// Event Management
//======================================

int sky_object_file_add_event(sky_object_file *object_file, sky_event *event)
{
    int rc = 0;
    sky_block_info *info = NULL;
    sky_block *block = NULL;
    
    sky_block **affected_blocks = NULL;
    int affected_block_count = 0;
    
    // Verify arguments.
    check(object_file != NULL, "Object file is required");
    check(event != NULL, "Event is required");
    check(object_file->state == SKY_OBJECT_FILE_STATE_LOCKED, "Object file must be locked to add events");
    
    // Make a copy of the event.
    sky_event *tmp = NULL;
    rc = sky_event_copy(event, &tmp);
    check(rc == 0, "Unable to copy event before insertion");
    event = tmp;
    
    // Find the appropriate block to insert into.
    rc = find_insertion_block(object_file, event, &info);
    check(rc == 0, "Unable to find an insertion block");

    // Load block to memory and deserialize.
    rc = load_block(object_file, info, &block);
    check(rc == 0, "Unable to load block %d", info->id);
    
    // Add event to block.
    rc = sky_block_add_event(block, event);
    check(rc == 0, "Unable to add event to block %d", info->id);
    
    // Attempt to split block into multiple blocks if necessary.
    rc = split_block(block, &affected_blocks, &affected_block_count);
    check(rc == 0, "Unable to split block %d", info->id);

    // Save all affected blocks.
    int i;
    for(i=0; i<affected_block_count; i++) {
        rc = save_block(affected_blocks[i]);
        check(rc == 0, "Unable to save block %d", affected_blocks[i]->info->id);
    }

    // Re-sort blocks.
    sort_blocks(object_file->infos, object_file->block_count);

    // Save header.
    save_header(object_file);

    // Clean up.
    sky_block_free(block);
    
    return 0;

error:
    // TODO: Restore state of database if an error occurred.
    sky_block_free(block);
    return -1;
}
