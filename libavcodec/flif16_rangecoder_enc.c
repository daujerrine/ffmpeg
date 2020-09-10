/*
 * This file shouldn't be posted as a patch. It is to be merged with
 * flif16_rangecoder.c.
 */

#include "flif16_rangecoder_enc.h"
#define PRINT_LINE printf("At: %s, %s %d\n", __func__, __FILE__, __LINE__);

/**
 * Initializes the range encoder.
 */
void ff_flif16_rac_enc_init(FLIF16RangeCoder *rc, PutByteContext *pb)
{
    rc->range          = FLIF16_RAC_MAX_RANGE;
    rc->low            = 0;
    rc->straddle_byte  = -1;
    rc->straddle_count = 0;
    rc->bytestream     = pb;
    printf("init range = %ld low = %ld sb = %d sc = %d\n",
           rc->range, rc->low, rc->straddle_byte, rc->straddle_count);
}

int ff_flif16_rac_enc_renorm(FLIF16RangeCoder *rc)
{
    int byte;
    while (rc->range <= FLIF16_RAC_MIN_RANGE) {
        printf("range = %ld low = %ld sb = %d sc = %d\n",
           rc->range, rc->low, rc->straddle_byte, rc->straddle_count);
        byte = rc->low >> FLIF16_RAC_MIN_RANGE_BITS;
        if (!bytestream2_get_bytes_left_p(rc->bytestream))
            return 0;
        if (rc->straddle_byte < 0) { // first generated byte
            rc->straddle_byte = byte;
        } else if (((rc->low + rc->range) >> 8) < FLIF16_RAC_MIN_RANGE) { // definitely no overflow
            bytestream2_put_byte(rc->bytestream, rc->straddle_byte);
            while (rc->straddle_count) {
                bytestream2_put_byte(rc->bytestream, 0xFF);
                rc->straddle_count--;
            }
            rc->straddle_byte = byte;
        } else if ((rc->low >> 8) >= FLIF16_RAC_MIN_RANGE) { // definitely overflow
            bytestream2_put_byte(rc->bytestream, rc->straddle_byte + 1);
            while (rc->straddle_count) {
                bytestream2_put_byte(rc->bytestream, 0x00);
                rc->straddle_count--;
            }
            rc->straddle_byte = byte & 0xFF;
        } else {
            rc->straddle_count++;
        }
        rc->low = (rc->low & (FLIF16_RAC_MIN_RANGE - 1)) << 8;
        rc->range <<= 8;
    }

    return 1;
}

int ff_flif16_rac_enc_flush(FLIF16RangeCoder *rc)
{
    rc->low += FLIF16_RAC_MIN_RANGE - 1;

    rc->segment = 0;
    switch (rc->segment) {
    default:
        rc->segment = 1;
        for (int i = 0; i < 4; ++i) {
            rc->range = FLIF16_RAC_MIN_RANGE - 1;
    case 1:
            if (!ff_flif16_rac_enc_renorm(rc))
                return 0;
        }
    }

    rc->segment = 0;
    return 1;
}

int ff_flif16_rac_enc_put(FLIF16RangeCoder *rc, uint32_t chance,
                          uint8_t bit)
{
    printf("put range = %ld low = %ld sb = %d sc = %d\n",
           rc->range, rc->low, rc->straddle_byte, rc->straddle_count);
    printf("bit = %d chance = %d\n", bit, chance);
    if (bit) {
        rc->low += rc->range - chance;
        rc->range = chance;
    } else
        rc->range -= chance;

    return 1;
}

int ff_flif16_rac_enc_write_bit(FLIF16RangeCoder *rc, uint8_t bit)
{
    ff_flif16_rac_enc_put(rc, rc->range >> 1, bit);
    return 1;
}

int ff_flif16_rac_enc_write_uni_int(FLIF16RangeCoder *rc, int min,
                                    int max, int val, int type)
{
    int med;
    if (!rc->active) {
        rc->active = 1;
        rc->min    = min;
        rc->max    = max;
        rc->val    = val;
    }
    printf("min: %d max: %d, val: %d\n",
           rc->min, rc->max, rc->val);

    if (rc->min != 0) {
        printf("a\n");
        rc->max -= rc->min;
        rc->val -= rc->min;
    }

    if (rc->max == 0) {
        printf("b\n");
        rc->active = 0;
        return 1;
    }

    med = rc->max / 2;

    if (rc->val > med) {
        printf("c\n");
        ff_flif16_rac_enc_write_bit(rc, 1);
        rc->min = med + 1;
    } else {
        printf("d\n");
        ff_flif16_rac_enc_write_bit(rc, 0);
        rc->min = 0;
        rc->max = med;
    }
    return 0;
}

int ff_flif16_rac_enc_write_chance(FLIF16RangeCoder *rc,
                                   uint64_t b12, uint8_t bit)
{
    uint32_t ret = (rc->range * b12 + 0x800) >> 12;
    return ff_flif16_rac_enc_put(rc, ret, bit);
}

// NearZero Integer Coder

static inline int ff_flif16_rac_enc_nz_write_internal(FLIF16RangeCoder *rc,
                                                      FLIF16ChanceContext *ctx,
                                                      uint16_t type, uint8_t value)
{
    if(!ff_flif16_rac_enc_renorm(rc))
        return 0; // EAGAIN condition
    ff_flif16_rac_enc_write_chance(rc, ctx->data[type], value);
    ctx->data[type] = (!value) ? rc->ct.zero_state[ctx->data[type]]
                               : rc->ct.one_state[ctx->data[type]];
    return 1;
}

#define RAC_NZ_PUT(rc, ctx, chance, value)                                     \
    if (!ff_flif16_rac_enc_nz_write_internal((rc), (ctx), (chance),            \
                                            (uint8_t) (value))) {              \
        goto need_more_buffer;                                                 \
    }

int ff_flif16_rac_enc_write_nz_int(FLIF16RangeCoder *rc, FLIF16ChanceContext *ctx,
                                   int min, int max, int value)
{
    if (min == max)
        return 1;

    if (!rc->active) {
        rc->segment = 0;
        rc->amin    = 1;
        rc->active  = 1;
        rc->sign    = 0;
        rc->have    = 0;
    }

    switch (rc->segment) {
    case 0:
        RAC_NZ_PUT(rc, ctx, NZ_INT_ZERO, 0);
        rc->segment++;

        if (value == 0) { // value is zero
            return 1;
        }

        rc->sign = (value > 0 ? 1 : 0);

        if (max > 0 && min < 0) {
    case 1:
            RAC_NZ_PUT(rc, ctx, NZ_INT_SIGN, rc->sign);
        }
        rc->segment++;

        max      = (rc->sign ? 1 : -1);
        rc->e    = ff_log2(abs(value));
        rc->amin = rc->sign ? abs(min) : abs(max);
        rc->amax = rc->sign ? abs(max) : abs(min);

        rc->emax = ff_log2(rc->amax);
        rc->i    = ff_log2(rc->amin);

        while (rc->i < rc->emax) {
            // if exponent >i is impossible, we are done
            if ((1 << (rc->i + 1)) > rc->amax)
                break;
    case 2:
            RAC_NZ_PUT(rc, ctx, NZ_INT_EXP(((rc->e << 1) + rc->sign)), rc->i == rc->e);
            if (rc->i == rc->e)
                break;
            rc->i++;
        }

        rc->have = (1 << rc->e);
        rc->left = rc->have - 1;
        rc->segment++;

        for (rc->pos = rc->e; rc->pos > 0;) {
            rc->bit = 1;
            rc->left ^= (1 << (--rc->pos));
            rc->minabs1 = rc->have | (1 << rc->pos);
            rc->maxabs0 = rc->have | rc->left;
            if (rc->minabs1 > rc->amax) { // 1-bit is impossible
                rc->bit = 0;
            } else if (rc->maxabs0 >= rc->amin) { // 0-bit and 1-bit are both possible
                rc->bit = (abs(value) >> rc->pos) & 1;
    case 3:
                RAC_NZ_PUT(rc, ctx, NZ_INT_MANT(rc->pos), rc->bit);
            }
            rc->have |= (rc->bit << rc->pos);
        }
    }

    return 1;

    need_more_buffer:
    return 0;
}

int ff_flif16_rac_enc_write_gnz_int(FLIF16RangeCoder *rc,
                                    FLIF16ChanceContext *ctx,
                                    int min, int max, int value)
{
    int ret;
    if (min > 0) {
        ret = ff_flif16_rac_enc_write_nz_int(rc, ctx, 0, max - min, value - min);
    } else if (max < 0) {
        ret = ff_flif16_rac_enc_write_nz_int(rc, ctx, min - max, 0, value - max);
    } else
        ret = ff_flif16_rac_enc_write_nz_int(rc, ctx, min, max, value);

    return ret;
}



/*
void inline FinalCompoundSymbolBitCoder<BitChance,RAC,bits>::write(const bool bit, const SymbolChanceBitType type, const int i)
{
    BitChance& ch = chances.realChances.bit(type, i);
    rac.write_12bit_chance(ch.get_12bit(), bit);
    updateChances(type, i, bit);
}


template <typename BitChance, typename RAC, int bits> class CompoundSymbolBitCoder
{
public:
    typedef typename BitChance::Table Table;

private:
    const Table &table;
    RAC &rac;
    CompoundSymbolChances<BitChance, bits> &chances;
    std::vector<bool> &select;

    void inline updateChances(SymbolChanceBitType type, int i, bool bit)
    {
        BitChance& real = chances.realChances.bit(type,i);
        real.estim(bit, chances.realSize);
        real.put(bit, table);

        int8_t best_property = -1;
        uint64_t best_size = chances.realSize;
        for (unsigned int j=0; j<chances.virtChances.size(); j++) {
            BitChance& virt = (select)[j] ? chances.virtChances[j].first.bit(type,i)
                              : chances.virtChances[j].second.bit(type,i);
            virt.estim(bit, chances.virtSize[j]);
            virt.put(bit, table);
            if (chances.virtSize[j] < best_size) {
                best_size = chances.virtSize[j];
                best_property = j;
            }
        }
        chances.best_property = best_property;
    }
    BitChance inline & bestChance(SymbolChanceBitType type, int i = 0)
    {
        signed short int p = chances.best_property;
        return (p == -1 ? chances.realChances.bit(type,i)
                : ((select)[p] ? chances.virtChances[p].first.bit(type,i)
                   : chances.virtChances[p].second.bit(type,i) ));
    }

public:
    CompoundSymbolBitCoder(const Table &tableIn, RAC &racIn, CompoundSymbolChances<BitChance, bits> &chancesIn, std::vector<bool> &selectIn) : table(tableIn), rac(racIn), chances(chancesIn), select(selectIn) {}

    bool read(SymbolChanceBitType type, int i = 0)
    {
        BitChance& ch = bestChance(type, i);
        bool bit = rac.read_12bit_chance(ch.get_12bit());
        updateChances(type, i, bit);
        return bit;
    }

    void write(bool bit, SymbolChanceBitType type, int i = 0)
    {
        BitChance& ch = bestChance(type, i);
        rac.write_12bit_chance(ch.get_12bit(), bit);
        updateChances(type, i, bit);
    }
};


template <typename BitChance, typename RAC, int bits>
void FinalCompoundSymbolCoder<BitChance,RAC,bits>::write_int(FinalCompoundSymbolChances<BitChance, bits>& chancesIn, int min, int max, int val)
{
    FinalCompoundSymbolBitCoder<BitChance, RAC, bits> bitCoder(table, rac, chancesIn);
    writer<bits>(bitCoder, min, max, val);
}

template <typename BitChance, typename RAC, int bits>
void FinalCompoundSymbolCoder<BitChance,RAC,bits>::write_int(FinalCompoundSymbolChances<BitChance, bits>& chancesIn, int nbits, int val)
{
    FinalCompoundSymbolBitCoder<BitChance, RAC, bits> bitCoder(table, rac, chancesIn);
    writer(bitCoder, nbits, val);
}



template <typename BitChance, typename RAC, int bits> class CompoundSymbolCoder
{
private:
    typedef typename CompoundSymbolBitCoder<BitChance, RAC, bits>::Table Table;
    RAC &rac;
    const Table table;

public:

    CompoundSymbolCoder(RAC& racIn, int cut = 2, int alpha = 0xFFFFFFFF / 19) : rac(racIn), table(cut,alpha) {}

    int read_int(CompoundSymbolChances<BitChance, bits> &chancesIn, std::vector<bool> &selectIn, int min, int max)
    {
        if (min == max) {
            return min;
        }
        CompoundSymbolBitCoder<BitChance, RAC, bits> bitCoder(table, rac, chancesIn, selectIn);
        return reader<bits>(bitCoder, min, max);
    }

    void write_int(CompoundSymbolChances<BitChance, bits>& chancesIn, std::vector<bool> &selectIn, int min, int max, int val)
    {
        if (min == max) {
            assert(val==min);
            return;
        }
        CompoundSymbolBitCoder<BitChance, RAC, bits> bitCoder(table, rac, chancesIn, selectIn);
        writer<bits>(bitCoder, min, max, val);
    }

    int read_int(CompoundSymbolChances<BitChance, bits> &chancesIn, std::vector<bool> &selectIn, int nbits)
    {
        CompoundSymbolBitCoder<BitChance, RAC, bits> bitCoder(table, rac, chancesIn, selectIn);
        return reader(bitCoder, nbits);
    }

    void write_int(CompoundSymbolChances<BitChance, bits>& chancesIn, std::vector<bool> &selectIn, int nbits, int val)
    {
        CompoundSymbolBitCoder<BitChance, RAC, bits> bitCoder(table, rac, chancesIn, selectIn);
        writer(bitCoder, nbits, val);
    }
};



template <typename BitChance, typename RAC, int bits>
void FinalPropertySymbolCoder<BitChance,RAC,bits>::write_int(const Properties &properties, int min, int max, int val)
{
    if (min == max) {
        assert(val==min);
        return;
    }
    assert(properties.size() == nb_properties);
    FinalCompoundSymbolChances<BitChance,bits> &chances = find_leaf(properties);
    coder.write_int(chances, min, max, val);
}

template <typename BitChance, typename RAC, int bits>
void FinalPropertySymbolCoder<BitChance,RAC,bits>::write_int(const Properties &properties, int nbits, int val)
{
    assert(properties.size() == nb_properties);
    FinalCompoundSymbolChances<BitChance,bits> &chances = find_leaf(properties);
    coder.write_int(chances, nbits, val);
}


template <typename BitChance, typename RAC, int bits> class PropertySymbolCoder
{
public:
    typedef CompoundSymbolCoder<BitChance, RAC, bits> Coder;
private:
    RAC &rac;
    Coder coder;
    const Ranges range;
    unsigned int nb_properties;
    std::vector<CompoundSymbolChances<BitChance,bits> > leaf_node;
    Tree &inner_node;
    std::vector<bool> selection;
    int split_threshold;

    inline PropertyVal div_down(int64_t sum, int32_t count) const
    {
        assert(count > 0);
        if (sum >= 0) return sum/count;
        else return -((-sum + count-1)/count);
    }

    CompoundSymbolChances<BitChance,bits> inline &find_leaf(const Properties &properties)
    {
        uint32_t pos = 0;
        Ranges current_ranges = range;
        while(inner_node[pos].property != -1) {
//        e_printf("Checking property %i (val=%i, splitval=%i)\n",inner_node[pos].property,properties[inner_node[pos].property],inner_node[pos].splitval);
            if (properties[inner_node[pos].property] > inner_node[pos].splitval) {
                current_ranges[inner_node[pos].property].first = inner_node[pos].splitval + 1;
                pos = inner_node[pos].childID;
            } else {
                current_ranges[inner_node[pos].property].second = inner_node[pos].splitval;
                pos = inner_node[pos].childID+1;
            }
        }
//    fprintf(stdout,"Returning leaf node %i\n", inner_node[pos].childID);
        CompoundSymbolChances<BitChance,bits> &result = leaf_node[inner_node[pos].leafID];

        // split leaf node if some virtual context is performing (significantly) better
        if(result.best_property != -1
                && result.realSize > result.virtSize[result.best_property] + split_threshold
                && current_ranges[result.best_property].first < current_ranges[result.best_property].second) {

            int8_t p = result.best_property;
            PropertyVal splitval = div_down(result.virtPropSum[p],result.count);
            if (splitval >= current_ranges[result.best_property].second)
                splitval = current_ranges[result.best_property].second-1; // == does happen because of rounding and running average

            uint32_t new_inner = inner_node.size();
            inner_node.push_back(inner_node[pos]);
            inner_node.push_back(inner_node[pos]);
            inner_node[pos].splitval = splitval;
//            fprintf(stdout,"Splitting on property %i, splitval=%i (count=%i)\n",p,inner_node[pos].splitval, (int)result.count);
            inner_node[pos].property = p;
            if (result.count < INT16_MAX) inner_node[pos].count = result.count;
            else inner_node[pos].count = INT16_MAX;
            uint32_t new_leaf = leaf_node.size();
            result.resetCounters();
            leaf_node.push_back(CompoundSymbolChances<BitChance,bits>(result));
            uint32_t old_leaf = inner_node[pos].leafID;
            inner_node[pos].childID = new_inner;
            inner_node[new_inner].leafID = old_leaf;
            inner_node[new_inner+1].leafID = new_leaf;
            if (properties[p] > inner_node[pos].splitval) {
                return leaf_node[old_leaf];
            } else {
                return leaf_node[new_leaf];
            }
        }
        return result;
    }

    void inline set_selection_and_update_property_sums(const Properties &properties, CompoundSymbolChances<BitChance,bits> &chances)
    {
        chances.count++;
        for(unsigned int i=0; i<nb_properties; i++) {
            assert(properties[i] >= range[i].first);
            assert(properties[i] <= range[i].second);
            chances.virtPropSum[i] += properties[i];
            PropertyVal splitval = div_down(chances.virtPropSum[i],chances.count);
            selection[i] = (properties[i] > splitval);
        }
    }

public:
    PropertySymbolCoder(RAC& racIn, Ranges &rangeIn, Tree &treeIn, int st=CONTEXT_TREE_SPLIT_THRESHOLD, int cut = 2, int alpha = 0xFFFFFFFF / 19) :
        rac(racIn),
        coder(racIn, cut, alpha),
        range(rangeIn),
        nb_properties(range.size()),
        leaf_node(1,CompoundSymbolChances<BitChance,bits>(nb_properties)),
        inner_node(treeIn),
        selection(nb_properties,false),
        split_threshold(st)
    {
    }

    int read_int(Properties &properties, int min, int max)
    {
        CompoundSymbolChances<BitChance,bits> &chances = find_leaf(properties);
        set_selection_and_update_property_sums(properties,chances);
        CompoundSymbolChances<BitChance,bits> &chances2 = find_leaf(properties);
        return coder.read_int(chances2, selection, min, max);
    }

    void write_int(Properties &properties, int min, int max, int val)
    {
        CompoundSymbolChances<BitChance,bits> &chances = find_leaf(properties);
        set_selection_and_update_property_sums(properties,chances);
        CompoundSymbolChances<BitChance,bits> &chances2 = find_leaf(properties);
        coder.write_int(chances2, selection, min, max, val);
    }

    int read_int(Properties &properties, int nbits)
    {
        CompoundSymbolChances<BitChance,bits> &chances = find_leaf(properties);
        set_selection_and_update_property_sums(properties,chances);
        CompoundSymbolChances<BitChance,bits> &chances2 = find_leaf(properties);
        return coder.read_int(chances2, selection, nbits);
    }

    void write_int(Properties &properties, int nbits, int val)
    {
        CompoundSymbolChances<BitChance,bits> &chances = find_leaf(properties);
        set_selection_and_update_property_sums(properties,chances);
        CompoundSymbolChances<BitChance,bits> &chances2 = find_leaf(properties);
        coder.write_int(chances2, selection, nbits, val);
    }

    // destructive simplification procedure, prunes subtrees with too low counts
    long long int simplify_subtree(int pos, int divisor, int min_size, int indent, int plane)
    {
        PropertyDecisionNode &n = inner_node[pos];
        if (n.property == -1) {
            for (int i=0; i<indent; i++) v_printf(10,"  ");
            v_printf(10,"* leaf: count=%lli, size=%llu bits, bits per int: %f\n", (long long int)leaf_node[n.leafID].count, (unsigned long long int)leaf_node[n.leafID].realSize/5461, (leaf_node[n.leafID].count > 0 ? leaf_node[n.leafID].realSize/leaf_node[n.leafID].count*1.0/5461 : -1));
            if (leaf_node[n.leafID].count == 0) return -100; // avoid empty leafs by giving them an extra penalty
            return leaf_node[n.leafID].count;
        } else {
            for (int i=0; i<indent; i++) v_printf(10,"  ");
            v_printf(10,"* test: plane %i, property %i, value > %i ?  (after %lli steps)\n", plane, n.property, n.splitval, (long long int)n.count);
            long long int subtree_size = 0;
            subtree_size += simplify_subtree(n.childID, divisor, min_size, indent+1, plane);
            subtree_size += simplify_subtree(n.childID+1, divisor, min_size, indent+1, plane);
            n.count /= divisor;
            if (n.count > CONTEXT_TREE_MAX_COUNT) {
                n.count = CONTEXT_TREE_MAX_COUNT;
            }
            if (n.count < CONTEXT_TREE_MIN_COUNT) n.count=CONTEXT_TREE_MIN_COUNT;
            if (n.count > 0xf) n.count &= 0xfff8; // remove some lsb entropy
//            printf("%li COUNT\n",n.count);
            if (subtree_size < min_size) {
//                printf("  PRUNING THE ABOVE SUBTREE\n");
                n.property = -1; // procedure is destructive because the leafID is not set
            }
            return subtree_size;
        }
    }
*/

/*
template <typename BitChance, typename RAC>
void MetaPropertySymbolCoder<BitChance,RAC>::write_subtree(int pos, Ranges &subrange, const Tree &tree)
{
    const PropertyDecisionNode &n = tree[pos];
    int p = n.property;
    coder[0].write_int2(0,nb_properties,p+1);
    if (p != -1) {
        coder[1].write_int2(CONTEXT_TREE_MIN_COUNT, CONTEXT_TREE_MAX_COUNT, n.count);
//            printf("From properties 0..%i, split node at PROPERTY %i\n",nb_properties-1,p);
        int oldmin = subrange[p].first;
        int oldmax = subrange[p].second;
        assert(oldmin < oldmax);
        coder[2].write_int2(oldmin, oldmax-1, n.splitval);
//            e_printf( "Pos %i: prop %i splitval %i in [%i..%i]\n", pos, n.property, n.splitval, oldmin, oldmax-1);
        // > splitval
        subrange[p].first = n.splitval+1;
        write_subtree(n.childID, subrange, tree);

        // <= splitval
        subrange[p].first = oldmin;
        subrange[p].second = n.splitval;
        write_subtree(n.childID+1, subrange, tree);

        subrange[p].second = oldmax;
    }
}
template <typename BitChance, typename RAC>
void MetaPropertySymbolCoder<BitChance,RAC>::write_tree(const Tree &tree)
{
    //fprintf(stdout,"Saving tree with %lu nodes.\n",tree.size());
    Ranges rootrange(range);
    write_subtree(0, rootrange, tree);
}
*/


