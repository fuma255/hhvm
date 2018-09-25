/*
   +----------------------------------------------------------------------+
   | HipHop for PHP                                                       |
   +----------------------------------------------------------------------+
   | Copyright (c) 2010-present Facebook, Inc. (http://www.facebook.com)  |
   +----------------------------------------------------------------------+
   | This source file is subject to version 3.01 of the PHP license,      |
   | that is bundled with this package in the file LICENSE, and is        |
   | available through the world-wide-web at the following url:           |
   | http://www.php.net/license/3_01.txt                                  |
   | If you did not receive a copy of the PHP license and are unable to   |
   | obtain it through the world-wide-web, please send a note to          |
   | license@php.net so we can mail you a copy immediately.               |
   +----------------------------------------------------------------------+
*/
#ifndef incl_HPHP_ALIAS_CLASS_H_
#define incl_HPHP_ALIAS_CLASS_H_

#include "hphp/runtime/base/rds.h"

#include "hphp/runtime/vm/minstr-state.h"

#include "hphp/runtime/vm/jit/alias-id-set.h"
#include "hphp/runtime/vm/jit/stack-offsets.h"

#include <folly/Optional.h>

#include <bitset>
#include <string>
#include <cstdint>

namespace HPHP { struct StringData; }
namespace HPHP { namespace jit {

struct SSATmp;

//////////////////////////////////////////////////////////////////////

/*
 * AliasClass represents a lattice of abstract memory locations.  These support
 * type-system-like operations (operator<= for subset, operator| for union, and
 * maybe() to test for non-zero intersections).
 *
 * AUnknown is the top of the lattice (is the class of all possible memory
 * locations we care about).  AEmpty is the bottom.  AUnknownTV is a union of
 * all the classes that contain TypedValue-style storage of PHP values.  The
 * various special location types AFoo all have an AFooAny, which is the
 * superset of all AFoos.
 *
 * Part of the lattice currently looks like this:
 *
 *                         Unknown
 *                            |
 *                            |
 *                    +-------+-------+----------+--------------+
 *                    |               |          |              |
 *                 UnknownTV      IterPosAny  IterBaseAny  ClsRefSlotAny
 *                    |               |          |              |
 *                    |              ...        ...            ...
 *                    |
 *      +---------+---+---------------+-------------------------+----+
 *      |         |                   |                         |    |
 *      |         |                   |                         |    |
 *      |         |                   |                         | RdsAny
 *      |         |                   |                         |    |
 *      |         |                HeapAny*                     |   ...
 *      |         |                   |                         |
 *      |         |            +------+------+---------+        |
 *      |         |            |             |         |        |
 *   FrameAny  StackAny     ElemAny       PropAny   RefAny  MIStateAny
 *      |         |          /    \          |         |        |
 *     ...       ...   ElemIAny  ElemSAny   ...       ...       |
 *                        |         |                           |
 *                       ...       ...    +---------+--------+--+------+
 *                                        |         |        |         |
 *                                   MITempBase  MITvRef  MITvRef2     |
 *                                                                     |
 *                                                                     |
 *                                                  +--------+---------+
 *                                                  |        |
 *                                               MIBase**  MIPropS**
 *
 *
 *   (*) AHeapAny contains some things other than ElemAny, PropAny and RefAny
 *       that don't have explicit nodes in the lattice yet.  (Like the
 *       lvalBlackhole, etc.)  It's hard for this to matter to client code for
 *       now because we don't expose an intersection or difference operation.
 *
 *  (**) MIBase is a pointer, and MIPropS is an encoded value, so neither is
 *       UnknownTV, but its hard to find the right spot for them in this
 *       diagram.
 */
struct AliasClass;

//////////////////////////////////////////////////////////////////////

/*
 * Special data for locations known to be a set of locals on the frame `fp'.
 */
struct AFrame { SSATmp* fp; AliasIdSet ids; };

/*
 * Iterator state. Note that iterator slots can contain different kinds of data
 * (normal iterators, mutable iterators, or cuf "iterators"). However, in a well
 * formed program, each iterator slot will always contain a known type at any
 * given point without the possibility of aliasing. Therefore its safe to
 * consider the different types as totally distinct alias classes.
 */

/*
 * A specific php iterator's position value (m_pos).
 */
struct AIterPos  { SSATmp* fp; uint32_t id; };

/*
 * A specific php iterator's base and initialization state, for non-mutable
 * iterators.
 *
 * Instances of this AliasClass cover both the memory storing the pointer to
 * the object being iterated, and the initialization flags (itype and next
 * helper)---the reason for this is that nothing may load/store the
 * initialization state if it isn't also going to load/store the base pointer.
 */
struct AIterBase { SSATmp* fp; uint32_t id; };

/*
 * A specific CufIter's func value (m_func).
 */
struct ACufIterFunc { SSATmp* fp; uint32_t id; };

/*
 * A specific CufIter's ctx value (m_ctx).
 */
struct ACufIterCtx { SSATmp* fp; uint32_t id; };

/*
 * A specific CufIter's invoke name value (m_name).
 */
struct ACufIterInvName { SSATmp* fp; uint32_t id; };

/*
 * A specific CufIter's dynamic value (m_dynamic).
 */
struct ACufIterDynamic { SSATmp* fp; uint32_t id; };

/*
 * A location inside of an object property, with base `obj' and byte offset
 * `offset' from the ObjectData*.
 */
struct AProp  { SSATmp* obj; uint32_t offset; };

/*
 * A integer index inside of an array, with base `arr'.  The `arr' tmp is any
 * kind of array (not necessarily kPackedKind or whatnot).
 */
struct AElemI { SSATmp* arr; int64_t idx; };

/*
 * A location inside of an array, with base `arr', with a string key.  The
 * `arr' tmp is any kind of array.
 */
struct AElemS { SSATmp* arr; const StringData* key; };

/*
 * A range of the stack, starting at `offset' from the outermost frame pointer,
 * and extending `size' slots deeper into the stack (toward lower memory
 * addresses).  The frame pointer is the same for all stack ranges in the IR
 * unit, and thus is not stored here.  The reason ranges extend downward is
 * that it is common to need to refer to the class of all stack locations below
 * some depth (this can be done by putting INT32_MAX in the size).
 *
 * Some notes on how the evaluation stack is treated for alias analysis:
 *
 *   o Since AStack locations are always canonicalized, different AStack
 *     locations must not alias if there is no overlap in the ranges.
 *
 *   o In situations with inlined calls, we may in fact have AFrame locations
 *     that refer to the same concrete memory locations (in the evaluation
 *     stack) as other AStack locations in the same HHIR program.  However, the
 *     portions of the HHIR program that may use that memory location in either
 *     of those ways may not overlap, based on the HHBC semantics of function
 *     calls and eval stack usage.  (This may seem odd, but it's not really any
 *     different from knowing that a physical portion of the heap may be
 *     treated as both an AElemI or AProp, but not at the same time based on
 *     HHBC-level semantics.)
 */
struct AStack {
  // We can create an AStack from either a stack pointer or a frame pointer.
  // These constructors canonicalize the offset to be relative to the outermost
  // frame pointer.
  explicit AStack(SSATmp* fp, FPRelOffset offset, int32_t size);
  explicit AStack(SSATmp* sp, IRSPRelOffset offset, int32_t size);
  explicit AStack(FPRelOffset o, int32_t s) : offset(o), size(s) {}

  FPRelOffset offset;
  int32_t size;
};

/*
 * A RefData referenced by a BoxedCell.
 */
struct ARef { SSATmp* boxed; };


/*
 * A set of class-ref slots in the given frame.
 */
struct AClsRefClsSlot { SSATmp* fp; AliasIdSet ids; };
struct AClsRefTSSlot { SSATmp* fp; AliasIdSet ids; };

/*
 * A TypedValue stored in rds.
 *
 * Assumes this handle uniquely identifies a TypedValue in rds - it's
 * not required that the tv is at the start of the rds storage.
 */
struct ARds { rds::Handle handle; };

//////////////////////////////////////////////////////////////////////

struct AliasClass {
  enum rep : uint32_t {  // bits for various location classes
    BEmpty    = 0,
    // The relative order of the values are used in operator| to decide
    // which specialization is more useful.
    BFrame          = 1U << 0,
    BIterPos        = 1U << 1,
    BIterBase       = 1U << 2,
    BProp           = 1U << 3,
    BElemI          = 1U << 4,
    BElemS          = 1U << 5,
    BStack          = 1U << 6,
    BRef            = 1U << 7,
    BClsRefClsSlot  = 1U << 8,
    BClsRefTSSlot   = 1U << 9,
    BCufIterFunc    = 1U << 10,
    BCufIterCtx     = 1U << 11,
    BCufIterInvName = 1U << 12,
    BCufIterDynamic = 1U << 13,
    BRds            = 1U << 14,

    // Have no specialization, put them last.
    BMITempBase = 1U << 15,
    BMITvRef    = 1U << 16,
    BMITvRef2   = 1U << 17,
    BMIBase     = 1U << 18,
    BMIPropS    = 1U << 19,

    BElem      = BElemI | BElemS,
    BHeap      = BElem | BProp | BRef,
    BMIStateTV = BMITempBase | BMITvRef | BMITvRef2,
    BMIState   = BMIStateTV | BMIBase | BMIPropS,

    BIter      = BIterPos | BIterBase,
    BCufIter   = BCufIterFunc | BCufIterCtx | BCufIterInvName | BCufIterDynamic,
    BIterSlot  = BIter | BCufIter,

    BUnknownTV =
      ~(BIterSlot | BMIBase | BMIPropS | BClsRefClsSlot | BClsRefTSSlot),

    BUnknown   = static_cast<uint32_t>(-1),
  };

  /*
   * A hashing function for alias classes.
   */
  struct Hash { size_t operator()(AliasClass) const; };

  /*
   * Create an alias class from a bit representation.  Usually you should use
   * one of the other functions to create these.
   */
  explicit AliasClass(rep bits) : m_bits{bits} {}

  /*
   * Create an alias class with more precise specialized information about
   * where it is.
   */
  /* implicit */ AliasClass(AFrame);
  /* implicit */ AliasClass(AIterPos);
  /* implicit */ AliasClass(AIterBase);
  /* implicit */ AliasClass(ACufIterFunc);
  /* implicit */ AliasClass(ACufIterCtx);
  /* implicit */ AliasClass(ACufIterInvName);
  /* implicit */ AliasClass(ACufIterDynamic);
  /* implicit */ AliasClass(AProp);
  /* implicit */ AliasClass(AElemI);
  /* implicit */ AliasClass(AElemS);
  /* implicit */ AliasClass(AStack);
  /* implicit */ AliasClass(ARef);
  /* implicit */ AliasClass(AClsRefClsSlot);
  /* implicit */ AliasClass(AClsRefTSSlot);
  /* implicit */ AliasClass(ARds);

  /*
   * Exact equality.
   */
  bool operator==(AliasClass) const;
  bool operator!=(AliasClass o) const { return !(*this == o); }

  /*
   * Return an AliasClass that is the precise union of this class and another
   * class, or folly::none if that precise union cannot be represented.
   *
   * Guaranteed to be commutative.
   */
  folly::Optional<AliasClass> precise_union(AliasClass) const;

  /*
   * Create an alias class that is at least as big as the true union of this
   * alias class and another one.
   *
   * If this.precise_union(o) is not folly::none, this function is guaranteed
   * to return *this.precise_union(o).  Otherwise it may return an arbitrary
   * AliasClass bigger than the (unrepresentable) true union---callers should
   * not rely on specifics about how much bigger it is.
   *
   * Guaranteed to be commutative.
   */
  AliasClass operator|(AliasClass o) const;
  AliasClass& operator|=(AliasClass o) { return *this = *this | o; }

  /*
   * Returns whether this alias class is a non-strict-subset of another one.
   */
  bool operator<=(AliasClass) const;

  /*
   * Returns whether an alias class could possibly refer to the same concrete
   * memory location as another one.  Basically, do they have a non-empty
   * intersection.
   */
  bool maybe(AliasClass) const;

  /*
   * Returns whether the alias class contains a single location.
   */
  bool isSingleLocation() const;

  /*
   * Conditionally access specific known information of various kinds.
   *
   * Returns folly::none if this alias class has no specialization in that way.
   */
  folly::Optional<AFrame>          frame() const;
  folly::Optional<AIterPos>        iterPos() const;
  folly::Optional<AIterBase>       iterBase() const;
  folly::Optional<ACufIterFunc>    cufIterFunc() const;
  folly::Optional<ACufIterCtx>     cufIterCtx() const;
  folly::Optional<ACufIterInvName> cufIterInvName() const;
  folly::Optional<ACufIterDynamic> cufIterDynamic() const;
  folly::Optional<AProp>           prop() const;
  folly::Optional<AElemI>          elemI() const;
  folly::Optional<AElemS>          elemS() const;
  folly::Optional<AStack>          stack() const;
  folly::Optional<ARef>            ref() const;
  folly::Optional<AClsRefClsSlot>  clsRefClsSlot() const;
  folly::Optional<AClsRefTSSlot>   clsRefTSSlot() const;
  folly::Optional<ARds>            rds() const;

  /*
   * Conditionally access specific known information, but also checking that
   * that is the only major category contained in this AliasClass.
   *
   * I.e., cls.is_foo() is semantically equivalent to:
   *
   *   cls <= AFooAny ? cls.foo() : folly::none
   */
  folly::Optional<AFrame>          is_frame() const;
  folly::Optional<AIterPos>        is_iterPos() const;
  folly::Optional<AIterBase>       is_iterBase() const;
  folly::Optional<ACufIterFunc>    is_cufIterFunc() const;
  folly::Optional<ACufIterCtx>     is_cufIterCtx() const;
  folly::Optional<ACufIterInvName> is_cufIterInvName() const;
  folly::Optional<ACufIterDynamic> is_cufIterDynamic() const;
  folly::Optional<AProp>           is_prop() const;
  folly::Optional<AElemI>          is_elemI() const;
  folly::Optional<AElemS>          is_elemS() const;
  folly::Optional<AStack>          is_stack() const;
  folly::Optional<ARef>            is_ref() const;
  folly::Optional<AClsRefClsSlot>  is_clsRefClsSlot() const;
  folly::Optional<AClsRefTSSlot>   is_clsRefTSSlot() const;
  folly::Optional<ARds>            is_rds() const;

  /*
   * Like the other foo() and is_foo() methods, but since we don't have an
   * AMIState anymore, these return AliasClass instead.
   */
  folly::Optional<AliasClass> mis() const;
  folly::Optional<AliasClass> is_mis() const;

private:
  enum class STag {
    None,
    Frame,
    IterPos,
    IterBase,
    CufIterFunc,
    CufIterCtx,
    CufIterInvName,
    CufIterDynamic,
    Prop,
    ElemI,
    ElemS,
    Stack,
    Ref,
    ClsRefClsSlot,
    ClsRefTSSlot,
    Rds,

    IterBoth,  // A union of base and pos for the same iter.
    CufIterAll, // A union of all fields for the same CufIter.
  };
  struct UIterBoth   { SSATmp* fp; uint32_t id; };
  struct UCufIterAll { SSATmp* fp; uint32_t id; };
private:
  friend std::string show(AliasClass);
  friend AliasClass canonicalize(AliasClass);

  bool checkInvariants() const;
  bool equivData(AliasClass) const;
  bool subclassData(AliasClass) const;
  bool diffSTagSubclassData(rep relevant_bits, AliasClass) const;
  bool maybeData(AliasClass) const;
  bool diffSTagMaybeData(rep relevant_bits, AliasClass) const;
  folly::Optional<UIterBoth> asUIter() const;
  folly::Optional<UCufIterAll> asUCufIter() const;
  bool refersToSameIterHelper(AliasClass) const;
  bool refersToSameCufIterHelper(AliasClass) const;
  static folly::Optional<AliasClass>
    precise_diffSTag_unionData(rep newBits, AliasClass, AliasClass);
  static AliasClass unionData(rep newBits, AliasClass, AliasClass);
  static rep stagBits(STag tag);

private:
  rep m_bits;
  STag m_stag{STag::None};
  union {
    AFrame          m_frame;
    AIterPos        m_iterPos;
    AIterBase       m_iterBase;
    ACufIterFunc    m_cufIterFunc;
    ACufIterCtx     m_cufIterCtx;
    ACufIterInvName m_cufIterInvName;
    ACufIterDynamic m_cufIterDynamic;
    AProp           m_prop;
    AElemI          m_elemI;
    AElemS          m_elemS;
    AStack          m_stack;
    ARef            m_ref;
    AClsRefClsSlot  m_clsRefClsSlot;
    AClsRefTSSlot   m_clsRefTSSlot;
    ARds            m_rds;

    UIterBoth       m_iterBoth;
    UCufIterAll     m_cufIterAll;
  };
};

//////////////////////////////////////////////////////////////////////

/* General alias classes. */
auto const AEmpty             = AliasClass{AliasClass::BEmpty};
auto const AFrameAny          = AliasClass{AliasClass::BFrame};
auto const AIterPosAny        = AliasClass{AliasClass::BIterPos};
auto const AIterBaseAny       = AliasClass{AliasClass::BIterBase};
auto const ACufIterFuncAny    = AliasClass{AliasClass::BCufIterFunc};
auto const ACufIterCtxAny     = AliasClass{AliasClass::BCufIterCtx};
auto const ACufIterInvNameAny = AliasClass{AliasClass::BCufIterInvName};
auto const ACufIterDynamicAny = AliasClass{AliasClass::BCufIterDynamic};
auto const ACufIterAny        = AliasClass{AliasClass::BCufIter};
auto const APropAny           = AliasClass{AliasClass::BProp};
auto const AHeapAny           = AliasClass{AliasClass::BHeap};
auto const ARefAny            = AliasClass{AliasClass::BRef};
auto const AStackAny          = AliasClass{AliasClass::BStack};
auto const AClsRefClsSlotAny  = AliasClass{AliasClass::BClsRefClsSlot};
auto const AClsRefTSSlotAny   = AliasClass{AliasClass::BClsRefTSSlot};
auto const AClsRefSlotAny     = AClsRefClsSlotAny | AClsRefTSSlotAny;
auto const ARdsAny            = AliasClass{AliasClass::BRds};
auto const AElemIAny          = AliasClass{AliasClass::BElemI};
auto const AElemSAny          = AliasClass{AliasClass::BElemS};
auto const AElemAny           = AliasClass{AliasClass::BElem};
auto const AMIStateTV         = AliasClass{AliasClass::BMIStateTV};
auto const AMIStateAny        = AliasClass{AliasClass::BMIState};
auto const AUnknownTV         = AliasClass{AliasClass::BUnknownTV};
auto const AUnknown           = AliasClass{AliasClass::BUnknown};

/* Alias classes for specific MInstrState fields. */
auto const AMIStateTempBase   = AliasClass{AliasClass::BMITempBase};
auto const AMIStateTvRef      = AliasClass{AliasClass::BMITvRef};
auto const AMIStateTvRef2     = AliasClass{AliasClass::BMITvRef2};
auto const AMIStateBase       = AliasClass{AliasClass::BMIBase};
auto const AMIStatePropS      = AliasClass{AliasClass::BMIPropS};

//////////////////////////////////////////////////////////////////////

/*
 * Creates an AliasClass given an offset into MInstrState.
 */
AliasClass mis_from_offset(size_t);

/*
 * Replace any SSATmps in an AliasClass with their canonical name (chasing
 * passthrough instructions as with canonical() from analysis.h.)
 */
AliasClass canonicalize(AliasClass);

/*
 * Produce a debug string for an alias class.
 */
std::string show(AliasClass);

//////////////////////////////////////////////////////////////////////

}}

#endif
