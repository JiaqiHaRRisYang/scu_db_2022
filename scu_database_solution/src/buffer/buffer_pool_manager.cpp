#include "buffer/buffer_pool_manager.h"

namespace scudb {

/*
 * BufferPoolManager Constructor
 * When log_manager is nullptr, logging is disabled (for test purpose)
 * WARNING: Do Not Edit This Function
 */
    BufferPoolManager::BufferPoolManager(size_t pool_size,
                                         DiskManager *disk_manager,
                                         LogManager *log_manager)
            : pool_size_(pool_size), disk_manager_(disk_manager),
              log_manager_(log_manager) {
        // a consecutive memory space for buffer pool
        pages_ = new Page[pool_size_];
        page_table_ = new ExtendibleHash<page_id_t, Page *>(BUCKET_SIZE);
        replacer_ = new LRUReplacer<Page *>;
        free_list_ = new std::list<Page *>;

        // put all the pages into free list
        for (size_t i = 0; i < pool_size_; ++i) {
            free_list_->push_back(&pages_[i]);
        }
    }

/*
 * BufferPoolManager Deconstructor
 * WARNING: Do Not Edit This Function
 */
    BufferPoolManager::~BufferPoolManager() {
        delete[] pages_;
        delete page_table_;
        delete replacer_;
        delete free_list_;
    }

/* help function to get pointer of VictimPage
 *
 */
    Page *BufferPoolManager::GetVictimPage() {

        //获得VictimPage的Pointer，要么来自于free Page，要么来自于 lru换页后得到的
        Page *target = nullptr;
        if (free_list_->empty()) {
            // to find a free page for replacement
            //先考虑没有被
            //那么如果
            if (replacer_->Size() == 0) {
                // to find an unpinned page for replacement
                // LRU replacer也是空的
                return nullptr;
            }
            else {
                //如果replacer中出来了，那么直接选出
                replacer_->Victim(target);
            }

        } else {
            //直接选空闲页
            target = free_list_->front();
            free_list_->pop_front();
            assert(target->GetPageId() == INVALID_PAGE_ID);
        }
        assert(target->GetPinCount() == 0);
        return target;
    }


/**
 * Fetch 取页
 * 1. search hash table.
 *  1.1 if exist, pin the page and return immediately
 *  1.2 if no exist, find a replacement entry from either free list or lru
 *      replacer. (NOTE: always find from free list first)
 * 2. If the entry chosen for replacement is dirty, write it back to disk.
 * 3. Delete the entry for the old page from the hash table and insert an
 * entry for the new page.
 * 4. Update page metadata, read page content from disk file and return page
 * pointer
 */
    Page *BufferPoolManager::FetchPage(page_id_t page_id) {
        // 对整个buffer上锁
        lock_guard<mutex> lck(latch_);
        Page *targetPtr = nullptr;
        //* 1. search hash table.
        // *  1.1 if exist, pin the page and return immediately
        if (page_table_->Find(page_id, targetPtr)) {
            targetPtr->pin_count_++;
            replacer_->Erase(targetPtr);
            return targetPtr;
        } else {
            // *  1.2 if no exist, find a replacement entry from either free list or lru
            // *      replacer. (NOTE: always find from free list first)
            targetPtr = GetVictimPage();    //获得了avaliable frame page
            if (targetPtr == nullptr) return targetPtr;
            // * 2. If the entry chosen for replacement is dirty, write it back to disk.
            if (targetPtr->is_dirty_) {
                disk_manager_->WritePage(targetPtr->GetPageId(), targetPtr->data_);
            }
            // * 3. Delete the entry for the old page from the hash table and insert an
            // * entry for the new page.
            page_table_->Remove(targetPtr->GetPageId());
            page_table_->Insert(page_id, targetPtr);
            // * 4. Update page metadata, read page content from disk file and return page
            // * pointer
            disk_manager_->ReadPage(page_id, targetPtr->data_);
            targetPtr->pin_count_ = 1;
            targetPtr->is_dirty_ = false;
            targetPtr->page_id_ = page_id;
        }
        return targetPtr;
    }

/*
 * Implementation of unpin page
 * if pin_count>0, decrement it and if it becomes zero, put it back to
 * replacer if pin_count<=0 before this call, return false. is_dirty: set the
 * dirty flag of this page
 */
    bool BufferPoolManager::UnpinPage(page_id_t page_id, bool is_dirty) {
        lock_guard<mutex> lck(latch_);
        Page *targetPtr = nullptr;
        page_table_->Find(page_id, targetPtr);
        //是否找到
        if (targetPtr == nullptr) {
            return false;
        } else {
            targetPtr->is_dirty_ = is_dirty;
            if (targetPtr->GetPinCount() <= 0) {
                return false;
            }
            targetPtr->pin_count_--;
            if (targetPtr->pin_count_ == 0) {
                replacer_->Insert(targetPtr);
            }
            return true;
        }
    }

/*
 * Used to flush a particular page of the buffer pool to disk. Should call the
 * write_page method of the disk manager
 * if page is not found in page table, return false
 * NOTE: make sure page_id != INVALID_PAGE_ID
 */
    bool BufferPoolManager::FlushPage(page_id_t page_id) {
        // * Used to flush a particular page of the buffer pool to disk. Should call the
        lock_guard<mutex> lck(latch_);
        Page *targetPtr = nullptr;
        page_table_->Find(page_id, targetPtr);
        if (targetPtr == nullptr || targetPtr->page_id_ == INVALID_PAGE_ID) {
            // * if page is not found in page table, return false
            // * NOTE: make sure page_id != INVALID_PAGE_ID
            return false;
        } else {
            // * write_page method of the disk manager
            if (targetPtr->is_dirty_) {
                disk_manager_->WritePage(page_id, targetPtr->GetData());
                targetPtr->is_dirty_ = false;
            }
        }
        return true;
    }

/**
 * User should call this method for deleting a page. This routine will call
 * disk manager to deallocate the page.
 * First, if page is found within page table,
 * buffer pool manager should be reponsible for removing this entry out
 * of page table, reseting page metadata and adding back to free list. Second,
 * call disk manager's DeallocatePage() method to delete from disk file. If
 * the page is found within page table, but pin_count != 0, return false
 */
    bool BufferPoolManager::DeletePage(page_id_t page_id) {
        lock_guard<mutex> lck(latch_);
        Page *targetPtr = nullptr;
        page_table_->Find(page_id, targetPtr);
        if (targetPtr != nullptr) {
            //如果在页表中，removing this entry out of page table,
            // reseting page metadata and adding back to free list.
            if (targetPtr->GetPinCount() > 0) {
                return false;
            }
            replacer_->Erase(targetPtr);
            page_table_->Remove(page_id);
            targetPtr->is_dirty_ = false;
            targetPtr->ResetMemory();
            free_list_->push_back(targetPtr);
        }
        disk_manager_->DeallocatePage(page_id);
        return true;
    }

/**
 * User should call this method if needs to create a new page. This routine
 * will call disk manager to allocate a page.
 * Buffer pool manager should be responsible to choose a victim page either
 * from free list or lru replacer(NOTE: always choose from free list first),
 * update new page's metadata, zero out memory and add corresponding entry
 * into page table. return nullptr if all the pages in pool are pinned
 */
    Page *BufferPoolManager::NewPage(page_id_t &page_id) {
        lock_guard<mutex> lck(latch_);
        Page *targetPtr = nullptr;
        targetPtr = GetVictimPage();
        if (targetPtr == nullptr) {
            return nullptr;
        }
        page_id = disk_manager_->AllocatePage();

        if (targetPtr->is_dirty_){
            disk_manager_->WritePage(targetPtr->GetPageId(), targetPtr->data_);
        }
        page_table_->Remove(targetPtr->GetPageId());
        page_table_->Insert(page_id, targetPtr);

        targetPtr->page_id_ = page_id;
        targetPtr->ResetMemory();
        targetPtr->is_dirty_ = false;
        targetPtr->pin_count_ = 1;
        return targetPtr;
    }
} // namespace scudb