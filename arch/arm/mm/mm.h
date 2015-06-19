/* the upper-most page table pointer */

#ifdef CONFIG_MMU

extern pmd_t *top_pmd;

#define TOP_PTE(x)	pte_offset_kernel(top_pmd, x)

static inline pmd_t *pmd_off(pgd_t *pgd, unsigned long virt)
{
	return pmd_offset(pgd, virt);
}

static inline pmd_t *pmd_off_k(unsigned long virt)
{
	return pmd_off(pgd_offset_k(virt), virt);
}

struct mem_type {
	//页表项的访问控制权,pte即第二级映射表项(页表项)
	unsigned int prot_pte;
	//段表项的访问控制位,l1即第一级映射表项(段表项/主页表项)
	unsigned int prot_l1;
	//代表主页表(不是主页表项)的访问控制位和内存域
	unsigned int prot_sect;
	//所属的内存域
	unsigned int domain;
};

const struct mem_type *get_mem_type(unsigned int type);

#endif

struct map_desc;
struct meminfo;
struct pglist_data;

void __init create_mapping(struct map_desc *md);
void __init bootmem_init(struct meminfo *mi);
void reserve_node_zero(struct pglist_data *pgdat);

extern void _text, _stext, _etext, __data_start, _end, __init_begin, __init_end;
