#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <inttypes.h>
#include <string.h>
#include <errno.h>
#include <sys/mman.h>
#include <unistd.h>
#include <fuse.h>
#include <fuse_opt.h>

/*! Offset to file name table offset */
#define FNT_OFFSET 0x40

/*! Offset to file name table length */
#define FNT_LENGTH 0x44

/*! Offset to file allocation table offset */
#define FAT_OFFSET 0x48

/*! Offset to file allocation length */
#define FAT_LENGTH 0x4C

/*! NitroFS root directory ID */
#define NITRO_ROOT    0xF000

/*! NitroFS directory ID mask */
#define NITRO_DIRMASK 0x0FFF

/*! NitroFS entry type */
typedef enum
{
  NITRO_FILE_TYPE, /*!< File entry */
  NITRO_DIR_TYPE,  /*!< Directory entry */
} nitro_type_t;

/*! NitroFS directory mode (dr-xr-xr-x) */
#define NITRO_DIR_MODE  (S_IRUSR|S_IXUSR|S_IRGRP|S_IXGRP|S_IROTH|S_IXOTH|S_IFDIR)
/*! NitroFS file mode (-r--r--r--) */
#define NITRO_FILE_MODE (S_IRUSR|S_IRGRP|S_IROTH|S_IFREG)

/*! NDS file name */
static const char *nds_file = NULL;
/*! NDS file size */
static size_t     nds_size;

/*! NDS file last access time */
static time_t nds_atime;
/*! NDS file last modification time */
static time_t nds_mtime;
/*! NDS file last attribute change time */
static time_t nds_ctime;

/*! NDS file mmap address */
static unsigned char *nds_mapping;

/*! Filename table offset */
static uint32_t fnt_offset;
static uint32_t fnt_length;
static uint32_t fat_offset;
static uint32_t fat_length;

/*! Typedef for nitrofs_entry_t */
typedef struct nitrofs_entry_t nitrofs_entry_t;

/*! NitroFS entry */
struct nitrofs_entry_t
{
  nitrofs_entry_t *parent;   /*!< Pointer to parent entry */
  nitrofs_entry_t *next;     /*!< Pointer to next entry (sibling) */
  nitrofs_entry_t *children; /*!< Pointer to first child entry */
  nitro_type_t    type;      /*!< File or directory */
  uint32_t        size;      /*!< Entry size */
  uint32_t        links;     /*!< Number of links */
  uint16_t        id;        /*!< Entry ID */
  char            name[];    /*!< Entry name */
};

/*! Root entry */
static nitrofs_entry_t *root = NULL;

/*! Entry in the main FNT table */
typedef struct
{
  uint32_t offset;    /*!< Offset to sub FNT entry */
  uint16_t next_id;   /*!< Starting offset for files */
  uint16_t parent_id; /*!< ID of parent */
} fnt_main_entry_t;

/*! Entry in the FAT table */
typedef struct
{
  uint32_t start_offset; /*!< Data start offset */
  uint32_t end_offset;   /*!< Data end offset */
} fat_entry_t;

/*! Initialize a directory entry
 *
 *  @param[out] dir    Entry to fill
 *  @param[in]  parent Pointer to parent
 *  @param[in]  id     ID to set
 */
static void
nitro_init_dir(nitrofs_entry_t *dir,
               nitrofs_entry_t *parent,
               uint16_t        id)
{
  dir->type      = NITRO_DIR_TYPE;
  dir->id        = id;
  dir->size      = 0;
  dir->links     = 2; // . and ..
  dir->next      = NULL;
  dir->children  = NULL;
  dir->parent    = parent;
}

/*! Initialize a file entry
 *
 *  @param[out] file   Entry to fill
 *  @param[in]  parent Pointer to parent
 *  @param[in]  id     ID to set
 */
static void
nitro_init_file(nitrofs_entry_t *file,
                nitrofs_entry_t *parent,
                fat_entry_t     *fat_entry,
                uint16_t        id)
{
  file->type      = NITRO_FILE_TYPE;
  file->id        = id;
  file->size      = fat_entry->end_offset - fat_entry->start_offset;
  file->links     = 2; // . and ..
  file->next      = NULL;
  file->children  = NULL;
  file->parent    = parent;
}

/*! Fill a subdirectory with all of its children
 *
 *  @param[out] dir   Entry to fill
 *  @param[in]  entry FNT entry
 *
 *  @returns 0 for success
 */
static int
nitro_build_subdir(nitrofs_entry_t  *dir,
                   fnt_main_entry_t *entry)
{
  nitrofs_entry_t *next, **last = &dir->children;
  unsigned char   *p = nds_mapping + fnt_offset + entry->offset;
  uint16_t        next_id = entry->next_id;

  while(*p != 0)
  {
    /* length is lower 7 bits */
    size_t len = *p & 0x7F;

    /* allocate an entry */
    next = (nitrofs_entry_t*)malloc(sizeof(nitrofs_entry_t)+len+1);
    if(next == NULL)
      return -1;

    /* copy name into entry */
    memcpy(next->name, p+1, len);
    next->name[len] = 0;

    /* update 'last' */
    *last = next;
    last = &(*last)->next;

    if(*p & 0x80)
    {
      /* this is a directory entry */
      uint16_t id;
      fnt_main_entry_t entry;

      /* grab the ID which immediately follows the name */
      memcpy(&id, p + len + 1, sizeof(id));

      /* initialize the directory entry */
      nitro_init_dir(next, dir, id);

      /* update the parent stats */
      dir->links += 1;
      dir->size  += len + 3;

      /* copy the FNT entry */
      memcpy(&entry, nds_mapping + fnt_offset + ((id & NITRO_DIRMASK)*sizeof(entry)),
             sizeof(entry));

      /* recurse */
      if(nitro_build_subdir(next, &entry) != 0)
        return -1;

      /* account for extra space due to ID */
      p += 2;
    }
    else
    {
      /* this is a file entry */
      fat_entry_t fat_entry;

      /* copy the FAT entry */
      memcpy(&fat_entry, nds_mapping + fat_offset + (next_id * sizeof(fat_entry)),
             sizeof(fat_entry));

      /* initialize the file entry */
      nitro_init_file(next, dir, &fat_entry, next_id);

      /* update the parent stats */
      dir->size  += len + 1;

      /* increment the File ID */
      ++next_id;
    }

    /* position to next entry */
    p += len + 1;
  }

  return 0;
}

/*! Destroy a tree
 *
 *  @param[in] dir Tree to destroy
 */
static void
nitro_destroy_tree(nitrofs_entry_t *dir)
{
  nitrofs_entry_t *child, *next;

  /* iterate through the children */
  for(child = dir->children; child != NULL; child = next)
  {
    next = child->next;

    /* recurse */
    nitro_destroy_tree(child);
  }

  /* clean up self */
  free(dir);
}

/*! Build a tree
 *
 *  @returns 0 for success
 */
static int
nitro_build_tree(void)
{
  fnt_main_entry_t entry;

  /* allocate root node */
  root = (nitrofs_entry_t*)malloc(sizeof(nitrofs_entry_t));
  if(root == NULL)
    return -1;

  /* initialize root directory */
  nitro_init_dir(root, root, NITRO_ROOT);

  /* copy FNT entry */
  memcpy(&entry, nds_mapping + fnt_offset, sizeof(entry));

  /* fill in its children */
  if(nitro_build_subdir(root, &entry) != 0)
  {
    /* a failure; clean up */
    nitro_destroy_tree(root);
    return -1;
  }
  return 0;
}

/*! Fill a stat struct from an entry
 *
 *  @param[in]  entry Entry to use
 *  @param[out] st    Buffer to fill
 */
static void
nitro_fill_stat(nitrofs_entry_t *entry,
                struct stat     *st)
{
  st->st_dev     = 0;
  st->st_ino     = ((uint32_t)entry->parent->id << 8) | entry->id;
  st->st_nlink   = entry->links;
  st->st_uid     = getuid();
  st->st_gid     = getgid();
  st->st_rdev    = 0;
  st->st_size    = entry->size;
  st->st_blksize = 4096;
  st->st_blocks  = (st->st_size + st->st_blksize-1) / st->st_blksize;
  st->st_atime   = nds_atime;
  st->st_mtime   = nds_mtime;
  st->st_ctime   = nds_ctime;
  if(entry->type == NITRO_DIR_TYPE)
    st->st_mode = NITRO_DIR_MODE;
  else
    st->st_mode = NITRO_FILE_MODE;
}

/*! Traverse path to get entry
 *
 *  @param[in] path Path to traverse
 *
 *  @returns entry that was found
 *  @returns NULL for no entry
 */
static nitrofs_entry_t*
nitro_traverse_path(const char *path)
{
  const char      *p;
  nitrofs_entry_t *dir = root, *entry;

  /* special case; this is the root directory */
  if(strcmp(path, "/") == 0)
    return root;

  /* iterate through intermediate path components */
  p = strchr(++path, '/');
  while(p != NULL)
  {
    /* look at each child for a match */
    for(entry = dir->children; entry != NULL; entry = entry->next)
    {
      /* check if the name matches */
      if(strlen(entry->name) == p-path && memcmp(path, entry->name, p-path) == 0)
      {
        /* we found a match; stop looking at these children */
        dir = entry;
        break;
      }
    }

    /* move to the next component */
    path = ++p;
    p = strchr(path, '/');
  }

  /* we are at the final component; look at each child for a match */
  for(entry = dir->children; entry != NULL; entry = entry->next)
  {
    /* check if the name matches */
    if(strcmp(path, entry->name) == 0)
      return entry;
  }

  /* didn't find this entry */
  return NULL;
}

/*! Get attributes
 *
 *  @param[in]  path Path to lookup
 *  @param[out] st   Buffer to fill
 *
 *  @returns 0 for success
 *  @returns negated errno otherwise
 */
static int
nitro_getattr(const char  *path,
              struct stat *st)
{
  nitrofs_entry_t *entry;

  entry = nitro_traverse_path(path);
  if(entry == NULL)
    return -ENOENT;

  nitro_fill_stat(entry, st);
  return 0;
}

/*! Read a directory
 *
 *  @param[in]  path   Directory to read
 *  @param[out] buffer Buffer to fill
 *  @param[in]  filler Callback which fills buffer
 *  @param[in]  offset Directory offset
 *  @param[in]  fi     Open directory information
 *
 *  @returns 0 for success
 *  @returns negated errno otherwise
 */
static int
nitro_readdir(const char            *path,
              void                  *buffer,
              fuse_fill_dir_t       filler,
              off_t                 offset,
              struct fuse_file_info *fi)
{
  struct stat     st;
  off_t           off;

  /* we set up this entry pointer in nitro_opendir */
  nitrofs_entry_t *entry = (nitrofs_entry_t*)fi->fh;
  nitrofs_entry_t *child;

  /* offset 0 means '.' */
  if(offset == 0)
  {
    nitro_fill_stat(entry, &st);
    if(filler(buffer, ".", &st, ++offset))
      return 0;
  }

  /* offset 1 means '..' */
  if(offset == 1)
  {
    nitro_fill_stat(entry->parent, &st);
    if(filler(buffer, "..", &st, ++offset))
      return 0;
  }

  /* skip until we reach the desired offset */
  for(off = 2, child = entry->children; child != NULL; child = child->next, ++off)
  {
    if(off == offset)
    {
      /* we have reached the desired offset; start filling */
      nitro_fill_stat(child, &st);
      if(filler(buffer, child->name, &st, ++offset))
        return 0;
    }
  }

  return 0;;
}

/*! Open a file
 *
 *  @param[in]  path File to open
 *  @param[out] fi   Open file information
 *
 *  @returns 0 for success
 *  @returns negated errno otherwise
 */
static int
nitro_open(const char            *path,
           struct fuse_file_info *fi)
{
  nitrofs_entry_t *entry;

  /* lookup the path */
  entry = nitro_traverse_path(path);
  if(entry == NULL)
  {
    /* we didn't find it. if O_CREAT was specified, return EROFS */
    if(fi->flags & O_CREAT)
      return -EROFS;
    /* otherwise, return ENOENT */
    return -ENOENT;
  }

  /* don't allow write mode */
  if((fi->flags & O_ACCMODE) == O_RDWR)
    return -EACCES;
  if((fi->flags & O_ACCMODE) == O_WRONLY)
    return -EACCES;

  /* set the open file info to point to our found entry */
  fi->fh = (unsigned long)entry;
  return 0;
}

/*! Read a file
 *
 *  @param[in]  path   Path of open file
 *  @param[out] buffer Buffer to fill
 *  @param[in]  size   Size to fill
 *  @param[in]  offset Offset to start at
 *
 *  @returns number of bytes read
 *  @returns negated errno otherwise
 */
static int
nitro_read(const char            *path,
           char                  *buffer,
           size_t                size,
           off_t                 offset,
           struct fuse_file_info *fi)
{
  nitrofs_entry_t *entry = (nitrofs_entry_t*)fi->fh;
  fat_entry_t     fat_entry;

  if(offset < 0)
    return -EINVAL;

  /* past end-of-file; return 0 bytes read */
  if(offset >= entry->size)
    return 0;

  /* copy the FAT entry */
  memcpy(&fat_entry, nds_mapping + fat_offset + (entry->id * sizeof(fat_entry)),
         sizeof(fat_entry));

  /* if they want to read past end-of-file, truncate the amount to read */
  if(offset + size > entry->size)
    size = entry->size - offset;

  /* copy the data */
  memcpy(buffer, nds_mapping + fat_entry.start_offset, size);

  /* return number of bytes copied */
  return size;
}

/*! Open a directory
 *
 *  @param[in]  path Path to open
 *  @param[out] fi   Open directory information
 *
 *  @returns 0 for success
 *  @returns negated errno otherwise
 */
static int
nitro_opendir(const char            *path,
              struct fuse_file_info *fi)
{
  nitrofs_entry_t *entry;

  /* lookup the path */
  entry = nitro_traverse_path(path);
  if(entry == NULL)
    return -ENOENT;

  /* make sure this is a directory */
  if(entry->type != NITRO_DIR_TYPE)
    return -EISDIR;

  /* set the open directory info to point to our found entry */
  fi->fh = (unsigned long)entry;
  return 0;
}

/*! Cleanup after unmount
 *
 *  @param[in] data Unused
 */
static void
nitro_destroy(void *data)
{
  nitro_destroy_tree(root);
}

/*! NitroFS FUSE operations */
static const struct fuse_operations nitro_ops =
{
  .getattr          = nitro_getattr,
  .readdir          = nitro_readdir,
  .open             = nitro_open,
  .read             = nitro_read,
  .opendir          = nitro_opendir,
  .destroy          = nitro_destroy,
  .flag_nullpath_ok = 1,
  .flag_nopath      = 1,
};

/*! fuse_opt_parse callback
 *
 *  @param[in]  data    Unused
 *  @param[in]  arg     Current argument
 *  @param[in]  key     Argument key
 *  @param[out] outargs Unused
 *
 *  @returns 0 for success and discard
 *  @returns 1 for success and keep
 *  @returns -1 for failure
 */
static int
nitro_process_arg(void             *data,
                  const char       *arg,
                  int              key,
                  struct fuse_args *outargs)
{
  if(key == FUSE_OPT_KEY_NONOPT)
  {
    if(nds_file == NULL)
    {
      /* discard first non-option, which is the nds filename */
      nds_file = arg;
      return 0;
    }
  }
  return 1;
}

int main(int argc, char *argv[])
{
  struct fuse_args args = FUSE_ARGS_INIT(argc, argv);
  struct stat      st;
  int              fd, rc;

  /* parse options */
  if(fuse_opt_parse(&args, NULL, NULL, nitro_process_arg) != 0)
    return EXIT_FAILURE;
  if(nds_file == NULL)
    return EXIT_FAILURE;

  /* open the nds file */
  fd = open(nds_file, O_RDONLY);
  if(fd < 0)
  {
    perror("open");
    return EXIT_FAILURE;
  }

  /* get the file information */
  rc = fstat(fd, &st);
  if(rc != 0)
  {
    perror("fstat");
    close(fd);
    return EXIT_FAILURE;
  }

  /* set up global data about the nds file */
  nds_size = st.st_size;
  nds_atime = st.st_atime;
  nds_mtime = st.st_mtime;
  nds_ctime = st.st_ctime;

  /* mmap the nds file */
  nds_mapping = mmap(NULL, st.st_size, PROT_READ, MAP_PRIVATE, fd, 0);
  close(fd);
  if(nds_mapping == MAP_FAILED)
  {
    perror("mmap");
    return EXIT_FAILURE;
  }

  /* copy some more global data */
  memcpy(&fnt_offset, nds_mapping + FNT_OFFSET, sizeof(fnt_offset));
  memcpy(&fnt_length, nds_mapping + FNT_LENGTH, sizeof(fnt_length));
  memcpy(&fat_offset, nds_mapping + FAT_OFFSET, sizeof(fat_offset));
  memcpy(&fat_length, nds_mapping + FAT_LENGTH, sizeof(fat_length));

  /* build the nitro tree */
  if(nitro_build_tree() != 0)
    return EXIT_FAILURE;

  /* run the FUSE loop */
  rc = fuse_main(args.argc, args.argv, &nitro_ops, NULL);

  /* clean up */
  fuse_opt_free_args(&args);
  munmap(nds_mapping, st.st_size);

  return rc;
}