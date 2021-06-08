#include <linux/module.h>       /* Needed by all modules */
#include <linux/kernel.h>       /* Needed for KERN_INFO  */
#include <linux/init.h>         /* Needed for the macros */
#include <linux/fs.h>           /* libfs stuff           */
#include <linux/buffer_head.h>  /* buffer_head           */
#include <linux/slab.h>         /* kmem_cache            */
#include "assoofs.h"



/*
 * Otras funciones
 */
void assoofs_save_sb_info(struct super_block *vsb);
void assoofs_add_inode_info(struct super_block *sb, struct assoofs_inode_info *inode);
int assoofs_save_inode_info(struct super_block *sb, struct assoofs_inode_info *inode_info);
struct assoofs_inode_info *assoofs_search_inode_info(struct super_block *sb, struct assoofs_inode_info *start, struct assoofs_inode_info *search);

/*
 *  Operaciones sobre ficheros
 */
ssize_t assoofs_read(struct file * filp, char __user * buf, size_t len, loff_t * ppos);
ssize_t assoofs_write(struct file * filp, const char __user * buf, size_t len, loff_t * ppos);
const struct file_operations assoofs_file_operations = {
    .read = assoofs_read,
    .write = assoofs_write,
};

/**
 * Permite leer de un archivo
 * @param filp fichero a leer
 * @param __user direccion del buffer
 * @return tamaño de fichero
 */
ssize_t assoofs_read(struct file * filp, char __user * buf, size_t len, loff_t * ppos) {

    struct assoofs_inode_info *inode_info;
    struct buffer_head *bh;
    char *buffer;
    int nbytes;

    printk(KERN_INFO "Read request\n");

    //Reservamos memoria en cache para la informacion persistente
    inode_info = kmem_cache_alloc(assoofs_inode_cache, GFP_KERNEL);
    //Accedemos a la informacion persistente del archivo
    inode_info = filp->f_path.dentry->d_inode->i_private;

    //Combrobamos si hemos ppos es mayor que el tamaño del archivo
    if(*ppos >= inode_info->file_size){
	    return 0;
    }

    //Accedemos al contenido del archivo y lo guardamos en buffer
    bh = sb_bread(filp->f_path.dentry->d_inode->i_sb, inode_info->data_block_number);
    buffer = (char *) bh->b_data;

    //Copiamos los datos al buffer usuario para leerlos y devolvemos el numero de bytes leidos
    nbytes = min((size_t) inode_info->file_size, len);
    copy_to_user(buf, buffer, nbytes);
    *ppos += nbytes;
    return nbytes;
}

/**
 * Permite escribir en un archivo
 * @param filp archivo
 * @param __user direccion del buffer
 * @return longitud del archivo
 */
ssize_t assoofs_write(struct file * filp, const char __user * buf, size_t len, loff_t * ppos) {

    struct assoofs_inode_info *inode_info;
    struct buffer_head *bh;
    char *buffer;
    printk(KERN_INFO "Write request\n");
    //Accedemos a la informacion persistente del archivo
    //Reservamos memoria en cache para la informacion persistente
    inode_info = kmem_cache_alloc(assoofs_inode_cache, GFP_KERNEL);

    inode_info = filp->f_path.dentry->d_inode->i_private;
    
    //Accedemos al contenido del archivo
    bh = sb_bread(filp->f_path.dentry->d_inode->i_sb, inode_info->data_block_number);
    buffer = (char *) bh->b_data;
    
    //Copiamos al archivo
    buffer += *ppos;
    copy_from_user(buffer, buf, len);
    *ppos += len;

    //Marcamos el bloque como sucio y sincronizamos
    mark_buffer_dirty(bh);
    sync_dirty_buffer(bh);

    //Actualizamos el tamaño
    inode_info->file_size = *ppos;
    assoofs_save_inode_info(filp->f_path.dentry->d_inode->i_sb, inode_info);
    brelse(bh);
    return len;
}

/*
 *  Operaciones sobre directorios
 */
static int assoofs_iterate(struct file *filp, struct dir_context *ctx);
const struct file_operations assoofs_dir_operations = {
    .owner = THIS_MODULE,
    .iterate = assoofs_iterate,
};

/**
 * Permite mostrar el contenido de un directorio
 * @param filp directorio
 * @param ctx contexto
 * @return 0 o -1, si sale todo bien o no
 */
static int assoofs_iterate(struct file *filp, struct dir_context *ctx) {

    struct inode *inode;
    struct super_block *sb;
    struct assoofs_inode_info *inode_info;
    struct buffer_head *bh;
    struct assoofs_dir_record_entry *record;
    int i;

    printk(KERN_INFO "Iterate request\n");

    //Reservamos memoria en cache para la informacion persistente
    inode_info = kmem_cache_alloc(assoofs_inode_cache, GFP_KERNEL);

    //Accedemos al inodo y cogemos la parte persistente
    inode = filp->f_path.dentry->d_inode;
    sb = inode->i_sb;
    inode_info = inode->i_private;

    //Hacemos comprobaciones
    //Si la pos del contexto es distinto de 0
    if(ctx->pos){ 
	    return -1;
    }

    //Si el archivo no es un directorio
    if(!(S_ISDIR(inode_info->mode))){
	    return -1;
    }

    //Accedemos al bloque correspondiente donde se encuentra la informacion del directorio

    bh = sb_bread(sb, inode_info->data_block_number);
    record = (struct assoofs_dir_record_entry *) bh->b_data;
    for(i = 0; i < inode_info->dir_children_count ; i++){
        //Se añaden las entradas del directorio al contexto y se incrementa la posición
	    dir_emit(ctx, record->filename, ASSOOFS_FILENAME_MAXLEN, record->inode_no, DT_UNKNOWN);
	    ctx->pos += sizeof(struct assoofs_dir_record_entry);
	    record++;
    }
    brelse(bh);
    return 0;
}

/*
 *  Operaciones sobre inodos
 */
struct assoofs_inode_info *assoofs_get_inode_info(struct super_block *sb, uint64_t inode_no);
static struct inode *assoofs_get_inode(struct super_block *sb, int ino);
static int assoofs_create(struct inode *dir, struct dentry *dentry, umode_t mode, bool excl);
struct dentry *assoofs_lookup(struct inode *parent_inode, struct dentry *child_dentry, unsigned int flags);
static int assoofs_mkdir(struct inode *dir, struct dentry *dentry, umode_t mode);
int assoofs_sb_get_a_freeblock(struct super_block *sb, uint64_t *block);

static struct inode_operations assoofs_inode_ops = {
    .create = assoofs_create,
    .lookup = assoofs_lookup,
    .mkdir = assoofs_mkdir,
};

/**
 * Permite crear un inodo con la informacion persistente
 * @param sb superbloque
 * @param ino número de inodo en el almacen de inodos
 * @return struct inode con la informacion persistente del inodo numero ino
 */
static struct inode *assoofs_get_inode(struct super_block *sb, int ino){
    //Creamos el inodo
	struct inode *inode;
    struct assoofs_inode_info *inode_info;


    //Creamos nuevo inodo
	inode = new_inode(sb);
    //Reservamos memoria en cache para la informacion persistente
    inode_info = kmem_cache_alloc(assoofs_inode_cache, GFP_KERNEL);
	//Usamos la funcion auxiliar para conseguir la informacion del inodo en el almacen de inodos
	inode_info = assoofs_get_inode_info(sb, ino);

	//Asignamos las operaciones, el numero de inodo y toda la información al inodo creado
	if(S_ISDIR(inode_info->mode)){
		inode->i_fop = &assoofs_dir_operations;
	}else if(S_ISREG(inode_info->mode)){
		inode->i_fop = &assoofs_file_operations;
	}else{
		printk(KERN_ERR "Unknown inode type. Neither a directory nor a file\n");
	}
	inode->i_ino = inode_info->inode_no;
	inode->i_sb = sb;
	inode->i_op = &assoofs_inode_ops;
	inode->i_atime = inode->i_mtime = inode->i_ctime = current_time(inode);

	//Guardamos la informacion persistente del inodo
	inode->i_private = inode_info;

	return inode;

}
/**
 * Busca la entrada con el nombre (child_dentry->d_name.name) en el
 * directorio padre.
 * @param parent_inode contiene la informacion del directorio padre
 * @param child_dentry contiene la informacion de la entrada que se busca en el directorio padre
 * @param flags
 * @return NULL en cualquier caso
 */
struct dentry *assoofs_lookup(struct inode *parent_inode, struct dentry *child_dentry, unsigned int flags) {

    struct assoofs_inode_info *parent_info;
    struct super_block *sb = parent_inode->i_sb;
    struct buffer_head *bh;
    struct assoofs_dir_record_entry *record;
    int i;

    printk(KERN_INFO "Lookup request\n");

    //Reservamos memoria en cache para la informacion persistente
    parent_info = kmem_cache_alloc(assoofs_inode_cache, GFP_KERNEL);
    parent_info = parent_inode->i_private;

    //Accedemos al bloque de disco apuntado por parent_inode
    bh = sb_bread(sb, parent_info->data_block_number);

    //Recorremos el contenido buscando la entrada que corresponda al nombre
    record = (struct assoofs_dir_record_entry *) bh->b_data;
    for(i = 0; i < parent_info->dir_children_count; i++){
	    if(!strcmp(record->filename, child_dentry->d_name.name)){
		    struct inode *inode = assoofs_get_inode(sb, record->inode_no);
		    inode_init_owner(inode, parent_inode, ((struct assoofs_inode_info *)inode->i_private)->mode);
		    d_add(child_dentry, inode);
		    printk(KERN_INFO "Se ha encontrado la entrada");
		    return NULL;
	    }
	    record++;
    }
    printk(KERN_INFO "No se ha encontrado la entrada");
    return NULL;
}

/**
 * Permite crear inodos para archivos
 * @param dir inodo del directorio
 * @param dentry entrada del nuevo archivo
 * @param mode modo del nuevo archivo
 * @param excl
 * @return 0 si todo salio bien o sino devuelve un error
 */
static int assoofs_create(struct inode *dir, struct dentry *dentry, umode_t mode, bool excl) {
    //Estructura del inodo
    struct inode *inode;
    //Estructura que guarda la informacion persistente del inodo
    struct assoofs_inode_info *inode_info;
    struct assoofs_inode_info *parent_inode_info;
    struct assoofs_dir_record_entry *dir_contents;
    struct super_block *sb;
    uint64_t count;
    struct buffer_head *bh;
    int ret;

    printk(KERN_INFO "New file request\n");
    //Obtenemos un puntero al superbloque desde el directorio
    sb = dir->i_sb;
    //Obtenemos el numero de inodos en el sistema de archivos
    count = ((struct assoofs_super_block_info *)sb->s_fs_info)->inodes_count;
    

    //Creamos el nuevo inode y le asignamos sus atributos
    inode = new_inode(sb);
    inode->i_sb = sb;
    inode->i_atime = inode->i_mtime = inode->i_ctime = current_time(inode);
    inode->i_op = &assoofs_inode_ops;
    inode->i_ino = count + 1;
    if(inode->i_ino > ASSOOFS_MAX_FILESYSTEM_OBJECTS_SUPPORTED){
	    printk(KERN_ERR "No se admiten mas inodos.\n");
	    return -1;
    }

    //Reservamos memoria en cache para la informacion persistente
    inode_info = kmem_cache_alloc(assoofs_inode_cache, GFP_KERNEL);

    //Añadimos la informacion persistente al inodo
    inode_info->inode_no = inode->i_ino;
    inode_info->mode = mode;
    inode_info->file_size = 0;
    inode->i_private = inode_info;

    //Asignamos operaciones de fichero al inodo
    inode->i_fop = &assoofs_file_operations;

    //Asignamos propietario y permisos y añadimos al arbol de directorios el nuevo inodo
    inode_init_owner(inode, dir, mode);
    d_add(dentry, inode);

    //Comprobamos si quedan espacios libres
    ret = assoofs_sb_get_a_freeblock(sb, &inode_info->data_block_number);
    if(ret != 0){
	    printk(KERN_ERR "No quedan bloques libres");
	    return ret;
    }

    //Guardamos la informacion persistente en el disco
    assoofs_add_inode_info(sb, inode_info);

    //Añadimos la informacion del inodo al directorio padre
    //Reservamos memoria en cache para la informacion persistente
    parent_inode_info = kmem_cache_alloc(assoofs_inode_cache, GFP_KERNEL);
    parent_inode_info = dir->i_private;
    bh = sb_bread(sb, parent_inode_info->data_block_number);
    dir_contents = (struct assoofs_dir_record_entry *)bh->b_data;
    dir_contents += parent_inode_info->dir_children_count;
    dir_contents->inode_no = inode_info->inode_no;

    strcpy(dir_contents->filename, dentry->d_name.name);
    mark_buffer_dirty(bh);
    sync_dirty_buffer(bh);

    //Actualizamos la informacion persistente del inodo padre
    parent_inode_info->dir_children_count++;
    assoofs_save_inode_info(sb, parent_inode_info);
    brelse(bh);
    return 0;
}

/**
 * Permite encontrar y asignar un bloque libre accediendo al mapa de bits
 * @param sb superbloque
 * @param block puntero al numero de bloque de un inodo
 * @return 0 si todo salio bien sino devuelve -1
 */
int assoofs_sb_get_a_freeblock(struct super_block *sb, uint64_t *block){
    //Asignamos a assoofs_sb la informacion persistente del superbloque
	struct assoofs_super_block_info *assoofs_sb = sb->s_fs_info;
    int i;
	printk(KERN_INFO "Get free block request\n");
	//Se busca un bit que este a uno en el mapa de bits
	for(i = 2; i < ASSOOFS_MAX_FILESYSTEM_OBJECTS_SUPPORTED; i++){
		if(assoofs_sb->free_blocks & (1 << i))
			break;
	}

	//Se Comprueba el ultimo bloque para ver si quedan sitios libres
	if(!(assoofs_sb->free_blocks & (1 << i))){
		printk(KERN_ERR "No quedan bloques libres\n");
		return -1;
	}else {
		printk(KERN_INFO "Existen bloques libres\n");
	}
	//Se asigna el numero de bloque
	*block = i;
	assoofs_sb->free_blocks &= ~(1 << i);
	assoofs_save_sb_info(sb);
	return 0;
}

/**
 * Actualiza la información persistente del superbloque
 * @param vsb superbloque
 */
void assoofs_save_sb_info(struct super_block *vsb){
    struct buffer_head *bh;
    struct assoofs_super_block_info *sb;
    printk(KERN_INFO "Save superblock info request\n");
	sb = vsb->s_fs_info;
	bh = sb_bread(vsb, ASSOOFS_SUPERBLOCK_BLOCK_NUMBER);
	bh->b_data = (char *)sb;
	mark_buffer_dirty(bh);
	sync_dirty_buffer(bh);
	brelse(bh);
	printk(KERN_INFO "Guardado informacion persistente del sb en disco\n");
}

/**
 * Guarda en disco la informacion persistente del inodo
 * @param sb superbloque
 * @param inode informacion persistente del inodo
 */
void assoofs_add_inode_info(struct super_block *sb, struct assoofs_inode_info *inode){

    struct assoofs_super_block_info *assoofs_sb_info = sb->s_fs_info;
    struct assoofs_inode_info *inode_info;
    struct buffer_head *bh;

	printk(KERN_INFO "Add inode info request\n");

	//Leemos el bloque de los inodos en disco
	bh = sb_bread(sb, ASSOOFS_INODESTORE_BLOCK_NUMBER);

    //Reservamos memoria en cache para la informacion persistente
    inode_info = kmem_cache_alloc(assoofs_inode_cache, GFP_KERNEL);

	//Obtenemos un puntero al almacen y colocamos al final el nuevo inodo
	inode_info = (struct assoofs_inode_info *) bh->b_data;
	inode_info += assoofs_sb_info->inodes_count;
	memcpy(inode_info, inode, sizeof(struct assoofs_inode_info));

	//Lo marcamos como sucio y lo sincronizamos con el disco
	mark_buffer_dirty(bh);
	sync_dirty_buffer(bh);

	//Cambiamos el numero de inodos y guardamos la información del superbloque
	assoofs_sb_info->inodes_count++;
	assoofs_save_sb_info(sb);
	printk(KERN_INFO "Añadido la informacion persistente a disco\n");
}

/**
 * Actualizamos la informacion persistente del inodo
 * @param sb superbloque
 * @param inode_info informacion persistente del inodo
 * @return 0 si todo sale bien y -1 si se produce un error
 */
int assoofs_save_inode_info(struct super_block *sb, struct assoofs_inode_info *inode_info){
	struct assoofs_inode_info *inode_pos;
	struct assoofs_inode_info *inode_disk;
	struct buffer_head *bh;
	printk(KERN_INFO "Save inode info request\n");

	//Accedemos al almacen de inodos en disco
	bh = sb_bread(sb, ASSOOFS_INODESTORE_BLOCK_NUMBER);
	inode_disk = (struct assoofs_inode_info *)bh->b_data;
	//Buscamos el puntero a la informacion persistente del inodo en disco
	inode_pos = assoofs_search_inode_info(sb, inode_disk, inode_info);
	if(inode_pos == NULL){
		printk(KERN_ERR "Informacion del inodo no encontrado\n");
		return -1;
	}
	//Actualizamos el inodo y marcamos el bloque a sucio y lo sincronizamos
	memcpy(inode_pos, inode_info, sizeof(*inode_pos));
	mark_buffer_dirty(bh);
	sync_dirty_buffer(bh);

	printk(KERN_INFO "Guardado informacion persistente del nodo\n");
	brelse(bh);
	return 0;
}

/**
 * Buscamos el puntero a la informacion persistente de inodo
 * @param sb superbloque
 * @param start inodo donde empieza
 * @param search inodo que se busca
 * @return puntero a la informacion persistente del inodo
 */
struct assoofs_inode_info *assoofs_search_inode_info(struct super_block *sb, struct assoofs_inode_info *start, struct assoofs_inode_info *search){
	uint64_t count = 0;
	uint64_t inodes_count = ((struct assoofs_super_block_info *)sb->s_fs_info)->inodes_count;
	printk(KERN_INFO "Search inode info request\n");
	//Buscamos el inodo que tenga el mismo número de inodo
	while(start->inode_no != search->inode_no && count < inodes_count){
		count++;
		start++;
	}
	//Devolvemos el puntero si se ha encontrado el inodo que se busca
	if(start->inode_no == search->inode_no) {
	    printk(KERN_INFO "Se ha encontrado el inodo");
	    return start;
	}else {
	    printk(KERN_INFO "No se ha encontrado el inodo");
	    return NULL;
	}
}

/**
 * Permite crear inodos para directorios
 * @param dir Directorio donde se creará el nuevo directorio
 * @param dentry entrada del directorio en el padre
 * @param mode modo del directorio
 * @return 0 o -1 dependiendo de si sale bien
 */
static int assoofs_mkdir(struct inode *dir , struct dentry *dentry, umode_t mode) {

    //Estructura del inodo
    struct inode *inode;
    //Estructura que guarda la informacion persistente del inodo
    struct assoofs_inode_info *inode_info;
    struct assoofs_inode_info *parent_inode_info;
    struct assoofs_dir_record_entry *dir_contents;
    struct super_block *sb;
    uint64_t count;
    struct buffer_head *bh;
    int ret;

    printk(KERN_INFO "New directory request\n");
    //Obtenemos un puntero al superbloque desde el directorio
    sb = dir->i_sb;
    //Obtenemos el numero de inodos en el sistema de archivos
    count = ((struct assoofs_super_block_info *)sb->s_fs_info)->inodes_count;
    

    //Creamos el nuevo inode y le asignamos sus atributos
    inode = new_inode(sb);
    inode->i_sb = sb;
    inode->i_atime = inode->i_mtime = inode->i_ctime = current_time(inode);
    inode->i_op = &assoofs_inode_ops;
    inode->i_ino = count + 1;
    if(inode->i_ino > ASSOOFS_MAX_FILESYSTEM_OBJECTS_SUPPORTED){
	    printk(KERN_ERR "No se admiten mas inodos.\n");
	    return -1;
    }
    
    //Añadimos la informacion persistente al inodo
    inode_info = kmem_cache_alloc(assoofs_inode_cache, GFP_KERNEL);
    inode_info->inode_no = inode->i_ino;
    inode_info->mode = S_IFDIR | mode;
    inode_info->dir_children_count = 0;
    inode->i_private = inode_info;

    inode->i_fop = &assoofs_dir_operations;

    inode_init_owner(inode, dir, S_IFDIR | mode);
    d_add(dentry, inode);

    //Comprobamos si quedan espacios libres
    ret = assoofs_sb_get_a_freeblock(sb, &inode_info->data_block_number);
    if(ret != 0){
	    printk(KERN_ERR "No quedan bloques libres");
	    return ret;
    }

    //Guardamos la informacion persistente en el disco
    assoofs_add_inode_info(sb, inode_info);

    //Añadimos la informacion del inodo al directorio padre
    parent_inode_info = dir->i_private;
    bh = sb_bread(sb, parent_inode_info->data_block_number);
    dir_contents = (struct assoofs_dir_record_entry *)bh->b_data;
    dir_contents += parent_inode_info->dir_children_count;
    dir_contents->inode_no = inode_info->inode_no;

    //Marcamos el bloque como sucio y sincronizamos
    strcpy(dir_contents->filename, dentry->d_name.name);
    mark_buffer_dirty(bh);
    sync_dirty_buffer(bh);
    brelse(bh);

    //Actualizamos la informacion persistente del inodo padre
    parent_inode_info->dir_children_count++;
    assoofs_save_inode_info(sb, parent_inode_info);
    return 0;
}


/*
 * Liberar espacio de inodos
 */
int assoofs_destroy_inode(struct inode *inode){
    struct assoofs_inode *inode_info = inode->i_private;
    printk(KERN_INFO "Freeing private data of inode %p (%lu)\n", inode_info, inode->i_ino);
    kmem_cache_free(assoofs_inode_cache, inode_info);
    return 0;
}

/*
 *  Operaciones sobre el superbloque
 */
static const struct super_operations assoofs_sops = {
    //.drop_inode = generic_delete_inode,
    .drop_inode = assoofs_destroy_inode,
};


/**
 * Permite obtener la informacion persistente del inodo que esta el la posicion inode_no
 * @param sb el superbloque
 * @param inode_no numero de inodo en el almacen de inodos
 * @return assofs_inode_info con la información persistente del inodo
 */
struct assoofs_inode_info *assoofs_get_inode_info(struct super_block *sb, uint64_t inode_no){
    //Se accede a disco al almacen de inodos para conseguir la informacion de los inodos
	struct assoofs_inode_info *inode_info;
	struct buffer_head *bh;
    struct assoofs_super_block_info *afs_sb;
    struct assoofs_inode_info *buffer;
    int i;

    //Reservamos memoria en cache para la informacion persistente
    inode_info = kmem_cache_alloc(assoofs_inode_cache, GFP_KERNEL);

	bh = sb_bread(sb, ASSOOFS_INODESTORE_BLOCK_NUMBER);
	inode_info = (struct assoofs_inode_info *) bh->b_data;

	//Se recorre el almacén de inodos para encontrar el inodo con numero de inodo igual a inode_no
	afs_sb = sb->s_fs_info;
	buffer = NULL;

	for(i = 0; i < afs_sb->inodes_count; i++){
		if(inode_info->inode_no == inode_no){
			buffer = kmalloc(sizeof(struct assoofs_inode_info), GFP_KERNEL);
			memcpy(buffer, inode_info, sizeof(*buffer));
			break;
		}
		inode_info++;
	}

	//Se liberan los recursos y se devuelve la información del inodo
	brelse(bh);
	return buffer;
}

/*
 *  Inicialización del superbloque
 */
int assoofs_fill_super(struct super_block *sb, void *data, int silent) {
    struct buffer_head *bh;
    struct assoofs_super_block_info *assoofs_sb;
    struct inode *root_inode;


    printk(KERN_INFO "assoofs_fill_super request\n");
    // 1.- Leer la información persistente del superbloque del dispositivo de bloques

    bh = sb_bread(sb, ASSOOFS_SUPERBLOCK_BLOCK_NUMBER);
    assoofs_sb = (struct assoofs_super_block_info *) bh->b_data;
    

    // 2.- Comprobar los parámetros del superbloque
    if(assoofs_sb->magic == ASSOOFS_MAGIC){
	    printk(KERN_INFO "Numero magico de assoofs valido\n");
    }else{
	    printk(KERN_ERR "Numero magico invalido\n");
	    return -EPERM;
    }

    if(assoofs_sb->block_size == ASSOOFS_DEFAULT_BLOCK_SIZE){
	    printk(KERN_INFO "Tamaño de bloque correcto\n");
    }else {
	    printk(KERN_ERR "Tamaño de bloque incorrecto\n");
	    return -EPERM;
    }

    // 3.- Escribir la información persistente leída del dispositivo de bloques en el superbloque sb, incluído el campo s_op con las operaciones que soporta.
    sb->s_magic = assoofs_sb->magic;
    sb->s_maxbytes = assoofs_sb->block_size;
    sb->s_op = &assoofs_sops;
    sb->s_fs_info = assoofs_sb;
    // 4.- Crear el inodo raíz y asignarle operaciones sobre inodos (i_op) y sobre directorios (i_fop)

    root_inode = new_inode(sb);

    inode_init_owner(root_inode, NULL, S_IFDIR);
    
    root_inode->i_ino = ASSOOFS_ROOTDIR_INODE_NUMBER;
    root_inode->i_sb = sb;
    root_inode->i_op = &assoofs_inode_ops;
    root_inode->i_fop = &assoofs_dir_operations;
    root_inode->i_atime = root_inode->i_mtime = root_inode->i_ctime = current_time(root_inode);
    root_inode->i_private = assoofs_get_inode_info(sb, ASSOOFS_ROOTDIR_INODE_NUMBER);
    
    sb->s_root = d_make_root(root_inode);
    brelse(bh);
    return 0;
}

/*
 *  Montaje de dispositivos assoofs
 */
static struct dentry *assoofs_mount(struct file_system_type *fs_type, int flags, const char *dev_name, void *data) {
    struct dentry *ret;
    printk(KERN_INFO "assoofs_mount request\n");
    //TODO: ver si funciona el cambio a direccion assofs_fill_super argumento
    ret = mount_bdev(fs_type, flags, dev_name, data, &assoofs_fill_super);
    // Control de errores a partir del valor de ret. En este caso se puede utilizar la macro IS_ERR: if (IS_ERR(ret)) ...
    return ret;
}

/*
 *  assoofs file system type
 */
static struct file_system_type assoofs_type = {
    .owner   = THIS_MODULE,
    .name    = "assoofs",
    .mount   = assoofs_mount,
    .kill_sb = kill_litter_super,
};

static int __init assoofs_init(void) {
    int ret;
    printk(KERN_INFO "assoofs_init request\n");
    ret = register_filesystem(&assoofs_type);
    //Inicializar cache
    assoofs_inode_cache = kmem_cache_create("assoofs_inode_cache", sizeof(struct assoofs_inode_info), 0, (SLAB_RECLAIM_ACCOUNT | SLAB_MEM_SPREAD), NULL);
    // Control de errores a partir del valor de ret
    return ret;
}

static void __exit assoofs_exit(void) {
    int ret;
    printk(KERN_INFO "assoofs_exit request\n");
    ret = unregister_filesystem(&assoofs_type);
    //Liberar caché
    kmem_cache_destroy(assoofs_inode_cache);
    // Control de errores a partir del valor de ret
}

module_init(assoofs_init);
module_exit(assoofs_exit);
