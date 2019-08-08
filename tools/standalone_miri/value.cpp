/*
 * mrustc Standalone MIRI
 * - by John Hodge (Mutabah)
 *
 * value.cpp
 * - Runtime values
 */
#include "value.hpp"
#include "hir_sim.hpp"
#include "module_tree.hpp"
#include <iostream>
#include <iomanip>
#include <algorithm>
#include "debug.hpp"

namespace {
    static bool in_bounds(size_t ofs, size_t size, size_t max_size) {
        if( !(ofs < max_size) )
            return false;
        if( !(size <= max_size) )
            return false;
        return ofs + size <= max_size;
    }
};

::std::ostream& operator<<(::std::ostream& os, const Allocation* x)
{
    os << "A(#" << x->m_index << " " << x->tag() /*<< " +" << x->size()*/ << ")";
    return os;
}

FfiLayout FfiLayout::new_const_bytes(size_t s)
{
    return FfiLayout {
        { Range {s, true, false} }
        };
}
bool FfiLayout::is_valid_read(size_t o, size_t s) const
{
    for(const auto& r : ranges)
    {
        if( o < r.len ) {
            if( !r.is_valid )
                return false;
            if( o + s <= r.len )
            {
                s = 0;
                break;
            }
            s -= (r.len - o);
            o = 0;
        }
        else {
            o -= r.len;
        }
    }
    if( s > 0 )
    {
        return false;
    }
    return true;
}

uint64_t Allocation::s_next_index = 0;

AllocationHandle Allocation::new_alloc(size_t size, ::std::string tag)
{
    Allocation* rv = new Allocation();
    rv->m_index = s_next_index++;
    rv->m_tag = ::std::move(tag);
    rv->refcount = 1;
    rv->m_size = size;
    rv->m_data.resize( (size + 8-1) / 8 );    // QWORDS
    rv->m_mask.resize( (size + 8-1) / 8 );    // bitmap bytes
    //LOG_DEBUG(rv << " ALLOC");
    LOG_DEBUG(rv);
    return AllocationHandle(rv);
}
AllocationHandle::AllocationHandle(const AllocationHandle& x):
    m_ptr(x.m_ptr)
{
    if( m_ptr )
    {
        assert(m_ptr->refcount != 0);
        assert(m_ptr->refcount != SIZE_MAX);
        m_ptr->refcount += 1;
        //LOG_DEBUG(m_ptr << " REF++ " << m_ptr->refcount);
    }
}
AllocationHandle::~AllocationHandle()
{
    if( m_ptr )
    {
        m_ptr->refcount -= 1;
        //LOG_DEBUG(m_ptr << " REF-- " << m_ptr->refcount);
        if(m_ptr->refcount == 0)
        {
            delete m_ptr;
        }
    }
}

RelocationPtr RelocationPtr::new_alloc(AllocationHandle alloc)
{
    RelocationPtr   rv;
    auto* ptr = alloc.m_ptr;
    alloc.m_ptr = nullptr;
    rv.m_ptr = reinterpret_cast<void*>( reinterpret_cast<uintptr_t>(ptr) + static_cast<uintptr_t>(Ty::Allocation) );
    return rv;
}
RelocationPtr RelocationPtr::new_fcn(::HIR::Path p)
{
    RelocationPtr   rv;
    auto* ptr = new ::HIR::Path(::std::move(p));
    rv.m_ptr = reinterpret_cast<void*>( reinterpret_cast<uintptr_t>(ptr) + static_cast<uintptr_t>(Ty::Function) );
    return rv;
}
RelocationPtr RelocationPtr::new_string(const ::std::string* ptr)
{
    RelocationPtr   rv;
    rv.m_ptr = reinterpret_cast<void*>( reinterpret_cast<uintptr_t>(ptr) + static_cast<uintptr_t>(Ty::StdString) );
    return rv;
}
RelocationPtr RelocationPtr::new_ffi(FFIPointer info)
{
    RelocationPtr   rv;
    auto* ptr = new FFIPointer(info);
    rv.m_ptr = reinterpret_cast<void*>( reinterpret_cast<uintptr_t>(ptr) + static_cast<uintptr_t>(Ty::FfiPointer) );
    return rv;
}
RelocationPtr::RelocationPtr(const RelocationPtr& x):
    m_ptr(nullptr)
{
    if( x )
    {
        switch(x.get_ty())
        {
        case Ty::Allocation: {
            auto tmp = AllocationHandle( reinterpret_cast<Allocation*>(x.get_ptr()) );
            *this = RelocationPtr::new_alloc(tmp);
            tmp.m_ptr = nullptr;
            } break;
        case Ty::Function: {
            auto ptr_i = reinterpret_cast<uintptr_t>(new ::HIR::Path(x.fcn()));
            assert( (ptr_i & 3) == 0 );
            m_ptr = reinterpret_cast<void*>( ptr_i + static_cast<uintptr_t>(Ty::Function) );
            assert(get_ty() == Ty::Function);
            } break;
        case Ty::StdString:
            // No ownership semantics, just clone the pointer
            m_ptr = x.m_ptr;
            break;
        case Ty::FfiPointer: {
            auto ptr_i = reinterpret_cast<uintptr_t>(new FFIPointer(x.ffi()));
            assert( (ptr_i & 3) == 0 );
            m_ptr = reinterpret_cast<void*>( ptr_i + static_cast<uintptr_t>(Ty::FfiPointer) );
            assert(get_ty() == Ty::FfiPointer);
            } break;
        }
    }
    else
    {
        m_ptr = nullptr;
    }
}
RelocationPtr::~RelocationPtr()
{
    if( *this )
    {
        switch(get_ty())
        {
        case Ty::Allocation:
            (void)AllocationHandle( reinterpret_cast<Allocation*>(get_ptr()) );
            break;
        case Ty::Function: {
            auto* ptr = const_cast<::HIR::Path*>(&fcn());
            delete ptr;
            } break;
        case Ty::StdString: {
            // No ownership semantics
            } break;
        case Ty::FfiPointer: {
            auto* ptr = const_cast<FFIPointer*>(&ffi());
            delete ptr;
            } break;
        }
    }
}
size_t RelocationPtr::get_size() const
{
    if( !*this )
        return 0;
    switch(get_ty())
    {
    case Ty::Allocation:
        return alloc().size();
    case Ty::Function:
        return 0;
    case Ty::StdString:
        return str().size();
    case Ty::FfiPointer:
        return 0;
        //return ffi().size;
    }
    throw "Unreachable";
}

::std::ostream& operator<<(::std::ostream& os, const RelocationPtr& x)
{
    if( x )
    {
        switch(x.get_ty())
        {
        case RelocationPtr::Ty::Allocation:
            os << &x.alloc();
            break;
        case RelocationPtr::Ty::Function:
            os << x.fcn();
            break;
        case RelocationPtr::Ty::StdString:
            os << "\"" << x.str() << "\"";
            break;
        case RelocationPtr::Ty::FfiPointer:
            os << "FFI '" << x.ffi().tag_name << "' " << x.ffi().ptr_value;
            break;
        }
    }
    else
    {
        os << "null";
    }
    return os;
}

uint64_t ValueCommonRead::read_usize(size_t ofs) const
{
    uint64_t    v = 0;
    this->read_bytes(ofs, &v, POINTER_SIZE);
    return v;
}
void ValueCommonWrite::write_usize(size_t ofs, uint64_t v)
{
    this->write_bytes(ofs, &v, POINTER_SIZE);
}
void* ValueCommonRead::read_pointer_unsafe(size_t rd_ofs, size_t req_valid, size_t& out_size, bool& out_is_mut) const
{
    auto ofs = read_usize(rd_ofs);
    LOG_ASSERT(ofs >= Allocation::PTR_BASE, "Deref of invalid pointer");
    ofs -= Allocation::PTR_BASE;
    auto reloc = get_relocation(rd_ofs);
    if( !reloc )
    {
        if( ofs != 0 ) {
            LOG_FATAL("Read a non-zero offset with no relocation");
        }
        if( req_valid > 0 ) {
            LOG_ERROR("Attempting to read a null pointer");
        }
        out_is_mut = false;
        out_size = 0;
        return nullptr;
    }
    else
    {
        switch(reloc.get_ty())
        {
        case RelocationPtr::Ty::Allocation: {
            auto& a = reloc.alloc();
            LOG_ASSERT(in_bounds(ofs, req_valid, a.size()), "Out-of-bounds pointer (" << ofs << " + " << req_valid << " > " << a.size() << ")");
            a.check_bytes_valid( ofs, req_valid );
            out_size = a.size() - ofs;
            out_is_mut = true;
            return a.data_ptr() + ofs;
            }
        case RelocationPtr::Ty::StdString: {
            const auto& s = reloc.str();
            LOG_ASSERT(in_bounds(ofs, req_valid, s.size()), "Out-of-bounds pointer (" << ofs << " + " << req_valid << " > " << s.size() << ")");
            out_size = s.size() - ofs;
            out_is_mut = false;
            return const_cast<void*>( static_cast<const void*>(s.data() + ofs) );
            }
        case RelocationPtr::Ty::Function:
            LOG_FATAL("read_pointer w/ function");
        case RelocationPtr::Ty::FfiPointer: {
            const auto& f = reloc.ffi();
            size_t size = f.get_size();
            LOG_ASSERT(in_bounds(ofs, req_valid, size), "Out-of-bounds pointer (" << ofs << " + " << req_valid << " > " << size << ")");
            // TODO: Validity?
            //if( req_valid )
            //    LOG_FATAL("Can't request valid data from a FFI pointer");
            // TODO: Have an idea of mutability and available size from FFI
            out_size = size - ofs;
            out_is_mut = false;
            return reinterpret_cast<char*>(reloc.ffi().ptr_value) + ofs;
            }
        }
        throw "";
    }
}
ValueRef ValueCommonRead::read_pointer_valref_mut(size_t rd_ofs, size_t size)
{
    auto ofs = read_usize(rd_ofs);
    LOG_ASSERT(ofs >= Allocation::PTR_BASE, "Invalid pointer read");
    ofs -= Allocation::PTR_BASE;
    auto reloc = get_relocation(rd_ofs);
    LOG_DEBUG("ValueCommonRead::read_pointer_valref_mut(" << ofs << "+" << size << ", reloc=" << reloc << ")");
    if( !reloc )
    {
        LOG_ERROR("Getting ValRef to null pointer (no relocation)");
    }
    else
    {
        // Validate size and offset are in bounds
        switch(reloc.get_ty())
        {
        case RelocationPtr::Ty::Allocation:
            LOG_ASSERT( in_bounds(ofs, size,  reloc.alloc().size()), "Deref with OOB size - " << ofs << "+" << size << " > " << reloc.alloc().size() );
            break;
        case RelocationPtr::Ty::StdString:
            LOG_ASSERT( in_bounds(ofs, size,  reloc.str().size()), "Deref with OOB size - " << ofs << "+" << size << " > " << reloc.str().size() );
            break;
        case RelocationPtr::Ty::Function:
            LOG_FATAL("read_pointer_valref_mut w/ function");
        case RelocationPtr::Ty::FfiPointer:
            LOG_ASSERT( in_bounds(ofs, size,  reloc.ffi().get_size()), "Deref with OOB size - " << ofs << "+" << size << " > " << reloc.ffi().get_size() );
            break;
        }
        return ValueRef(reloc, ofs, size);
    }
}
ValueRef ValueCommonRead::deref(size_t ofs, const ::HIR::TypeRef& ty)
{
    return read_pointer_valref_mut(ofs, ty.get_size());
}


void Allocation::resize(size_t new_size)
{
    if( this->is_freed )
        LOG_ERROR("Use of freed memory " << this);
    //size_t old_size = this->size();
    //size_t extra_bytes = (new_size > old_size ? new_size - old_size : 0);

    this->m_size = new_size;
    this->m_data.resize( (new_size + 8-1) / 8 );
    this->m_mask.resize( (new_size + 8-1) / 8 );
}

void Allocation::check_bytes_valid(size_t ofs, size_t size) const
{
    if( !in_bounds(ofs, size, this->size()) ) {
        LOG_FATAL("Out of range - " << ofs << "+" << size << " > " << this->size());
    }
    for(size_t i = ofs; i < ofs + size; i++)
    {
        if( !(this->m_mask[i/8] & (1 << (i%8))) )
        {
            LOG_ERROR("Invalid bytes in value - " << ofs << "+" << size << " - " << *this);
            throw "ERROR";
        }
    }
}
void Allocation::mark_bytes_valid(size_t ofs, size_t size)
{
    assert( ofs+size <= this->m_mask.size() * 8 );
    for(size_t i = ofs; i < ofs + size; i++)
    {
        this->m_mask[i/8] |= (1 << (i%8));
    }
}
Value Allocation::read_value(size_t ofs, size_t size) const
{
    Value rv;
    //TRACE_FUNCTION_R("Allocation::read_value " << this << " " << ofs << "+" << size, *this << " | " << size << "=" << rv);
    if( this->is_freed )
        LOG_ERROR("Use of freed memory " << this);
    LOG_DEBUG(*this);
    LOG_ASSERT( in_bounds(ofs, size, this->size()), "Read out of bounds (" << ofs << "+" << size << " > " << this->size() << ")" );

    // Determine if this can become an inline allocation.
    bool has_reloc = false;
    for(const auto& r : this->relocations)
    {
        if( ofs <= r.slot_ofs && r.slot_ofs < ofs + size )
        {
            has_reloc = true;
        }
    }
    if( has_reloc || size > sizeof(rv.direct_data.data) )
    {
        rv.allocation = Allocation::new_alloc(size, FMT_STRING("Allocation::read_value(" << ofs << "," << size << ")"));

        rv.write_bytes(0, this->data_ptr() + ofs, size);

        for(const auto& r : this->relocations)
        {
            if( ofs <= r.slot_ofs && r.slot_ofs < ofs + size )
            {
                rv.allocation->relocations.push_back({ r.slot_ofs - ofs, r.backing_alloc });
            }
        }

        // Copy the mask bits
        for(size_t i = 0; i < size; i ++)
        {
            size_t j = ofs + i;
            const uint8_t test_mask = (1 << (j%8));
            const uint8_t set_mask = (1 << (i%8));
            bool v = (this->m_mask[j/8] & test_mask) != 0;
            if( v )
            {
                rv.allocation->m_mask[i/8] |= set_mask;
            }
        }
    }
    else
    {
        rv.direct_data.size = static_cast<uint8_t>(size);

        rv.write_bytes(0, this->data_ptr() + ofs, size);
        rv.direct_data.mask[0] = 0;
        rv.direct_data.mask[1] = 0;

        // Copy the mask bits
        for(size_t i = 0; i < size; i ++)
        {
            size_t j = ofs + i;
            const uint8_t tst_mask = 1 << (j%8);
            const uint8_t set_mask = 1 << (i%8);
            bool v = (this->m_mask[j/8] & tst_mask) != 0;
            if( v )
            {
                rv.direct_data.mask[i/8] |= set_mask;
            }
        }
    }
    return rv;
}
void Allocation::read_bytes(size_t ofs, void* dst, size_t count) const
{
    if( this->is_freed )
        LOG_ERROR("Use of freed memory " << this);

    //LOG_DEBUG("Allocation::read_bytes " << this << " " << ofs << "+" << count);
    if(count == 0)
        return ;

    if( !in_bounds(ofs, count, this->size()) ) {
        LOG_ERROR("Out of bounds read, " << ofs << "+" << count << " > " << this->size());
        throw "ERROR";
    }
    check_bytes_valid(ofs, count);


    ::std::memcpy(dst, this->data_ptr() + ofs, count);
}
void Allocation::write_value(size_t ofs, Value v)
{
    TRACE_FUNCTION_R("Allocation::write_value " << this << " " << ofs << "+" << v.size() << " " << v, *this);
    if( this->is_freed )
        LOG_ERROR("Use of freed memory " << this);
    //if( this->is_read_only )
    //    LOG_ERROR("Writing to read-only allocation " << this);
    if( v.allocation )
    {
        size_t  v_size = v.allocation->size();
        const auto& src_alloc = *v.allocation;
        // Take a copy of the source mask
        auto s_mask = src_alloc.m_mask;

        // Save relocations first, because `Foo = Foo` is valid.
        ::std::vector<Relocation>   new_relocs = src_alloc.relocations;
        // - write_bytes removes any relocations in this region.
        write_bytes(ofs, src_alloc.data_ptr(), v_size);

        // Find any relocations that apply and copy those in.
        // - Any relocations in the source within `v.meta.indirect_meta.offset` .. `v.meta.indirect_meta.offset + v_size`
        if( !new_relocs.empty() )
        {
            // 2. Move the new relocations into this allocation
            for(auto& r : new_relocs)
            {
                //LOG_TRACE("Insert " << r.backing_alloc);
                r.slot_ofs += ofs;
                this->relocations.push_back( ::std::move(r) );
            }
        }

        // Set mask in destination
        if( ofs % 8 != 0 || v_size % 8 != 0 )
        {
            // Lazy way, sets/clears individual bits
            for(size_t i = 0; i < v_size; i ++)
            {
                uint8_t dbit = 1 << ((ofs+i) % 8);
                if( s_mask[i/8] & (1 << (i %8)) )
                    this->m_mask[ (ofs+i) / 8 ] |= dbit;
                else
                    this->m_mask[ (ofs+i) / 8 ] &= ~dbit;
            }
        }
        else
        {
            // Copy the mask bytes directly
            for(size_t i = 0; i < v_size / 8; i ++)
            {
                this->m_mask[ofs/8+i] = s_mask[i];
            }
        }
    }
    else
    {
        this->write_bytes(ofs, v.direct_data.data, v.direct_data.size);

        // Lazy way, sets/clears individual bits
        for(size_t i = 0; i < v.direct_data.size; i ++)
        {
            uint8_t dbit = 1 << ((ofs+i) % 8);
            if( v.direct_data.mask[i/8] & (1 << (i %8)) )
                this->m_mask[ (ofs+i) / 8 ] |= dbit;
            else
                this->m_mask[ (ofs+i) / 8 ] &= ~dbit;
        }
    }
}
void Allocation::write_bytes(size_t ofs, const void* src, size_t count)
{
    //LOG_DEBUG("Allocation::write_bytes " << this << " " << ofs << "+" << count);
    if( this->is_freed )
        LOG_ERROR("Use of freed memory " << this);
    //if( this->is_read_only )
    //    LOG_ERROR("Writing to read-only allocation " << this);

    if(count == 0)
        return ;
    //TRACE_FUNCTION_R("Allocation::write_bytes " << this << " " << ofs << "+" << count, *this);
    if( !in_bounds(ofs, count, this->size()) ) {
        LOG_ERROR("Out of bounds write, " << ofs << "+" << count << " > " << this->size());
        throw "ERROR";
    }


    // - Remove any relocations already within this region
    auto& this_relocs = this->relocations;
    for(auto it = this_relocs.begin(); it != this_relocs.end(); )
    {
        if( ofs <= it->slot_ofs && it->slot_ofs < ofs + count)
        {
            //LOG_TRACE("Delete " << it->backing_alloc);
            it = this_relocs.erase(it);
        }
        else 
        {
            ++it;
        }
    }

    ::std::memcpy(this->data_ptr() + ofs, src, count);
    mark_bytes_valid(ofs, count);
}
void Allocation::write_ptr(size_t ofs, size_t ptr_ofs, RelocationPtr reloc)
{
    LOG_ASSERT(ptr_ofs >= Allocation::PTR_BASE, "Invalid pointer being written");
    this->write_usize(ofs, ptr_ofs);
    this->set_reloc(ofs, POINTER_SIZE, ::std::move(reloc));
}
void Allocation::set_reloc(size_t ofs, size_t len, RelocationPtr reloc)
{
    LOG_ASSERT(ofs % POINTER_SIZE == 0, "");
    LOG_ASSERT(len == POINTER_SIZE, "");
    // Delete any existing relocation at this position
    for(auto it = this->relocations.begin(); it != this->relocations.end();)
    {
        if( ofs <= it->slot_ofs && it->slot_ofs < ofs + len )
        {
            // Slot starts in this updated region
            // - TODO: Split in half?
            it = this->relocations.erase(it);
            continue ;
        }
        // TODO: What if the slot ends in the new region?
        // What if the new region is in the middle of the slot
        ++ it;
    }
    this->relocations.push_back(Relocation { ofs, /*len,*/ ::std::move(reloc) });
}
::std::ostream& operator<<(::std::ostream& os, const Allocation& x)
{
    auto flags = os.flags();
    os << ::std::hex;
    for(size_t i = 0; i < x.size(); i++)
    {
        if( i != 0 )
            os << " ";

        if( x.m_mask[i/8] & (1 << (i%8)) )
        {
            os << ::std::setw(2) << ::std::setfill('0') << (int)x.data_ptr()[i];
        }
        else
        {
            os << "--";
        }
    }
    os.setf(flags);

    os << " {";
    for(const auto& r : x.relocations)
    {
        if( 0 <= r.slot_ofs && r.slot_ofs < x.size() )
        {
            os << " @" << r.slot_ofs << "=" << r.backing_alloc;
        }
    }
    os << " }";
    return os;
}

Value::Value()
{
    this->direct_data.size = 0;
    memset(this->direct_data.mask, 0, sizeof(this->direct_data.mask));
}
Value::Value(::HIR::TypeRef ty)
{
    size_t size = ty.get_size();

    // Support inline data if the data will fit within the inline region (which is the size of the metadata)
    if( ty.get_size() <= sizeof(this->direct_data.data) )
    {
        // AND the type doesn't contain a pointer (of any kind)
        if( ! ty.has_pointer() )
        {
            // Will fit in a inline allocation, nice.
            //LOG_TRACE("No pointers in " << ty << ", storing inline");
            this->direct_data.size = static_cast<uint8_t>(size);
            this->direct_data.mask[0] = 0;
            this->direct_data.mask[1] = 0;
            return ;
        }
    }

    // Fallback: Make a new allocation
    //LOG_TRACE(" Creating allocation for " << ty);
    this->allocation = Allocation::new_alloc(size, FMT_STRING(ty));
}
Value Value::with_size(size_t size, bool have_allocation)
{
    Value   rv;
    if(have_allocation || size > sizeof(rv.direct_data.data))
    {
        rv.allocation = Allocation::new_alloc(size, FMT_STRING("with_size(" << size << ")"));
    }
    else
    {
        rv.direct_data.size = static_cast<uint8_t>(size);
        memset(rv.direct_data.mask, 0, sizeof(rv.direct_data.mask));
    }
    return rv;
}
Value Value::new_fnptr(const ::HIR::Path& fn_path)
{
    Value   rv( ::HIR::TypeRef(::HIR::CoreType { RawType::Function }) );
    assert(rv.allocation);
    rv.allocation->write_ptr(0, Allocation::PTR_BASE, RelocationPtr::new_fcn(fn_path));
    return rv;
}
Value Value::new_ffiptr(FFIPointer ffi)
{
    Value   rv( ::HIR::TypeRef(::HIR::CoreType { RawType::USize }) );
    rv.create_allocation();
    rv.allocation->write_ptr(0, Allocation::PTR_BASE, RelocationPtr::new_ffi(ffi));
    return rv;
}
Value Value::new_pointer(::HIR::TypeRef ty, uint64_t v, RelocationPtr r) {
    assert(ty.get_wrapper());
    assert(ty.get_wrapper()->type == TypeWrapper::Ty::Borrow || ty.get_wrapper()->type == TypeWrapper::Ty::Pointer);
    Value   rv(ty);
    rv.write_ptr(0, v, ::std::move(r));
    return rv;
}
Value Value::new_usize(uint64_t v) {
    auto rv = Value( ::HIR::TypeRef(RawType::USize) );
    rv.write_usize(0, v);
    return rv;
}
Value Value::new_isize(int64_t v) {
    auto rv = Value( ::HIR::TypeRef(RawType::ISize) );
    rv.write_isize(0, v);
    return rv;
}
Value Value::new_u32(uint32_t v) {
    auto rv = Value( ::HIR::TypeRef(RawType::U32) );
    rv.write_u32(0, v);
    return rv;
}
Value Value::new_i32(int32_t v) {
    auto rv = Value( ::HIR::TypeRef(RawType::I32) );
    rv.write_i32(0, v);
    return rv;
}
Value Value::new_i64(int64_t v) {
    auto rv = Value( ::HIR::TypeRef(RawType::I64) );
    rv.write_i64(0, v);
    return rv;
}

void Value::create_allocation()
{
    assert(!this->allocation);
    this->allocation = Allocation::new_alloc(this->direct_data.size, "create_allocation");
    if( this->direct_data.size > 0 )
        this->allocation->m_mask[0] = this->direct_data.mask[0];
    if( this->direct_data.size > 8 )
        this->allocation->m_mask[1] = this->direct_data.mask[1];
    ::std::memcpy(this->allocation->data_ptr(), this->direct_data.data, this->direct_data.size);
}
void Value::check_bytes_valid(size_t ofs, size_t size) const
{
    if( size == 0 )
        return ;
    if( this->allocation )
    {
        this->allocation->check_bytes_valid(ofs, size);
    }
    else
    {
        if( size == 0 && this->direct_data.size > 0 ) {
            return ;
        }
        if( !in_bounds(ofs, size, this->direct_data.size) ) {
            LOG_ERROR("Read out of bounds " << ofs+size << " >= " << int(this->direct_data.size));
            throw "ERROR";
        }
        for(size_t i = ofs; i < ofs + size; i++)
        {
            if( !(this->direct_data.mask[i/8] & (1 << i%8)) )
            {
                LOG_ERROR("Accessing invalid bytes in value");
                throw "ERROR";
            }
        }
    }
}
void Value::mark_bytes_valid(size_t ofs, size_t size)
{
    if( this->allocation )
    {
        this->allocation->mark_bytes_valid(ofs, size);
    }
    else
    {
        for(size_t i = ofs; i < ofs+size; i++)
        {
            this->direct_data.mask[i/8] |= (1 << i%8);
        }
    }
}

Value Value::read_value(size_t ofs, size_t size) const
{
    Value   rv;
    //TRACE_FUNCTION_R(ofs << ", " << size << ") - " << *this, rv);
    if( this->allocation )
    {
        rv = this->allocation->read_value(ofs, size);
    }
    else
    {
        // Inline always fits in inline.
        rv.direct_data.size = static_cast<uint8_t>(size);
        rv.write_bytes(0, this->direct_data.data+ofs, size);
        rv.direct_data.mask[0] = this->direct_data.mask[0];
        rv.direct_data.mask[1] = this->direct_data.mask[1];
    }
    return rv;
}
void Value::read_bytes(size_t ofs, void* dst, size_t count) const
{
    if(count == 0)
        return ;
    if( this->allocation )
    {
        this->allocation->read_bytes(ofs, dst, count);
    }
    else
    {
        check_bytes_valid(ofs, count);

        // TODO: Redundant due to above?
        if( !in_bounds(ofs, count, this->direct_data.size) ) {
            LOG_ERROR("Out of bounds read, " << ofs << "+" << count << " > " << this->size());
            throw "ERROR";
        }
        ::std::memcpy(dst, this->direct_data.data + ofs, count);
    }
}

void Value::write_bytes(size_t ofs, const void* src, size_t count)
{
    if( count == 0 )
        return ;
    if( this->allocation )
    {
        this->allocation->write_bytes(ofs, src, count);
    }
    else
    {
        if( !in_bounds(ofs, count, this->direct_data.size) ) {
            LOG_BUG("Write extends outside value size (" << ofs << "+" << count << " >= " << (int)this->direct_data.size << ")");
        }
        ::std::memcpy(this->direct_data.data + ofs, src, count);
        mark_bytes_valid(ofs, count);
    }
}
void Value::write_value(size_t ofs, Value v)
{
    if( this->allocation )
    {
        this->allocation->write_value(ofs, ::std::move(v));
    }
    else
    {
        if( v.allocation && !v.allocation->relocations.empty() )
        {
            this->create_allocation();
            this->allocation->write_value(ofs, ::std::move(v));
        }
        else
        {
            write_bytes(ofs, v.direct_data.data, v.direct_data.size);

            // Lazy way, sets/clears individual bits
            for(size_t i = 0; i < v.direct_data.size; i ++)
            {
                uint8_t dbit = 1 << ((ofs+i) % 8);
                if( v.direct_data.mask[i/8] & (1 << (i %8)) )
                    this->direct_data.mask[ (ofs+i) / 8 ] |= dbit;
                else
                    this->direct_data.mask[ (ofs+i) / 8 ] &= ~dbit;
            }
        }
    }
}
void Value::write_ptr(size_t ofs, size_t ptr_ofs, RelocationPtr reloc)
{
    if( !this->allocation )
    {
        LOG_ERROR("Writing a pointer with no allocation");
    }
    this->allocation->write_ptr(ofs, ptr_ofs, ::std::move(reloc));
}

::std::ostream& operator<<(::std::ostream& os, const Value& v)
{
    if( v.allocation )
    {
        os << *v.allocation;
    }
    else
    {
        auto flags = os.flags();
        os << ::std::hex;
        for(size_t i = 0; i < v.direct_data.size; i++)
        {
            if( i != 0 )
                os << " ";
            if( v.direct_data.mask[i/8] & (1 << i%8) )
            {
                os << ::std::setw(2) << ::std::setfill('0') << (int)v.direct_data.data[i];
            }
            else
            {
                os << "--";
            }
        }
        os.setf(flags);
    }
    return os;
}
extern ::std::ostream& operator<<(::std::ostream& os, const ValueRef& v)
{
    if( v.m_size == 0 )
        return os;
    if( v.m_alloc )
    {
        const auto& alloc_ptr = v.m_alloc;;
        // TODO: What if alloc_ptr isn't a data allocation?
        switch(alloc_ptr.get_ty())
        {
        case RelocationPtr::Ty::Allocation: {
            const auto& alloc = alloc_ptr.alloc();

            os << &alloc << "@" << v.m_offset << "+" << v.m_size << " ";

            auto flags = os.flags();
            os << ::std::hex;
            for(size_t i = v.m_offset; i < ::std::min(alloc.size(), v.m_offset + v.m_size); i++)
            {
                if( i != 0 )
                    os << " ";

                if( alloc.m_mask[i/8] & (1 << i%8) )
                {
                    os << ::std::setw(2) << ::std::setfill('0') << (int)alloc.data_ptr()[i];
                }
                else
                {
                    os << "--";
                }
            }
            os.setf(flags);

            os << " {";
            for(const auto& r : alloc.relocations)
            {
                if( v.m_offset <= r.slot_ofs && r.slot_ofs < v.m_offset + v.m_size )
                {
                    os << " @" << (r.slot_ofs - v.m_offset) << "=" << r.backing_alloc;
                }
            }
            os << " }";
            } break;
        case RelocationPtr::Ty::Function:
            LOG_TODO("ValueRef to " << alloc_ptr);
            break;
        case RelocationPtr::Ty::StdString: {
            const auto& s = alloc_ptr.str();
            assert( in_bounds(v.m_offset, v.m_size, s.size()) );
            auto flags = os.flags();
            os << ::std::hex;
            for(size_t i = v.m_offset; i < v.m_offset + v.m_size; i++)
            {
                os << ::std::setw(2) << ::std::setfill('0') << (int)s.data()[i];
            }
            os.setf(flags);
            } break;
        case RelocationPtr::Ty::FfiPointer:
            LOG_TODO("ValueRef to " << alloc_ptr);
            break;
        }
    }
    else if( v.m_value && v.m_value->allocation )
    {
        const auto& alloc = *v.m_value->allocation;

        os << &alloc << "@" << v.m_offset << "+" << v.m_size << " ";

        auto flags = os.flags();
        os << ::std::hex;
        for(size_t i = v.m_offset; i < ::std::min(alloc.size(), v.m_offset + v.m_size); i++)
        {
            if( i != 0 )
                os << " ";

            if( alloc.m_mask[i/8] & (1 << i%8) )
            {
                os << ::std::setw(2) << ::std::setfill('0') << (int)alloc.data_ptr()[i];
            }
            else
            {
                os << "--";
            }
        }
        os.setf(flags);

        os << " {";
        for(const auto& r : alloc.relocations)
        {
            if( v.m_offset <= r.slot_ofs && r.slot_ofs < v.m_offset + v.m_size )
            {
                os << " @" << (r.slot_ofs - v.m_offset) << "=" << r.backing_alloc;
            }
        }
        os << " }";
    }
    else if( v.m_value )
    {
        const auto& direct = v.m_value->direct_data;

        auto flags = os.flags();
        os << ::std::hex;
        for(size_t i = v.m_offset; i < ::std::min(static_cast<size_t>(direct.size), v.m_offset + v.m_size); i++)
        {
            if( i != 0 )
                os << " ";
            if( direct.mask[i/8] & (1 << i%8) )
            {
                os << ::std::setw(2) << ::std::setfill('0') << (int)direct.data[i];
            }
            else
            {
                os << "--";
            }
        }
        os.setf(flags);
    }
    else
    {
        // TODO: no value?
    }
    return os;
}

void ValueRef::mark_bytes_valid(size_t ofs, size_t size)
{
    if( m_alloc ) {
        switch(m_alloc.get_ty())
        {
        case RelocationPtr::Ty::Allocation:
            m_alloc.alloc().mark_bytes_valid(m_offset + ofs, size);
            break;
        default:
            LOG_TODO("mark_valid in " << m_alloc);
        }
    }
    else {
        m_value->mark_bytes_valid(m_offset + ofs, size);
    }
}

Value ValueRef::read_value(size_t ofs, size_t size) const
{
    if( size == 0 )
        return Value();
    if( !in_bounds(ofs, size,  m_size) ) {
        LOG_ERROR("Read exceeds bounds, " << ofs << " + " << size << " > " << m_size << " - from " << *this);
    }
    if( m_alloc ) {
        switch(m_alloc.get_ty())
        {
        case RelocationPtr::Ty::Allocation:
            return m_alloc.alloc().read_value(m_offset + ofs, size);
        case RelocationPtr::Ty::StdString: {
            LOG_ASSERT(in_bounds(m_offset + ofs, size, m_alloc.str().size()), "");
            auto rv = Value::with_size(size, false);
            rv.write_bytes(0, m_alloc.str().data() + m_offset + ofs, size);
            return rv;
            }
        case RelocationPtr::Ty::FfiPointer: {
            LOG_ASSERT(in_bounds(m_offset + ofs, size, m_alloc.ffi().get_size()), "");
            auto rv = Value::with_size(size, false);
            rv.write_bytes(0, reinterpret_cast<const char*>(m_alloc.ffi().ptr_value) + m_offset + ofs, size);
            return rv;
            }
        default:
            LOG_TODO("read_value from " << m_alloc);
        }
    }
    else {
        return m_value->read_value(m_offset + ofs, size);
    }
}
bool ValueRef::compare(size_t offset, const void* other, size_t other_len) const
{
    check_bytes_valid(offset, other_len);
    return ::std::memcmp(data_ptr() + offset, other, other_len) == 0;
}
