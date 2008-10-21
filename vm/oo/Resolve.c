/*
 * Copyright (C) 2008 The Android Open Source Project
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *      http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */
/*
 * Resolve classes, methods, fields, and strings.
 *
 * According to the VM spec (v2 5.5), classes may be initialized by use
 * of the "new", "getstatic", "putstatic", or "invokestatic" instructions.
 * If we are resolving a static method or static field, we make the
 * initialization check here.
 *
 * (NOTE: the verifier has its own resolve functions, which can be invoked
 * if a class isn't pre-verified.  Those functions must not update the
 * "resolved stuff" tables for static fields and methods, because they do
 * not perform initialization.)
 */
#include "Dalvik.h"

#include <stdlib.h>


/*
 * Find the class corresponding to "classIdx", which maps to a class name
 * string.  It might be in the same DEX file as "referrer", in a different
 * DEX file, generated by a class loader, or generated by the VM (e.g.
 * array classes).
 *
 * Because the DexTypeId is associated with the referring class' DEX file,
 * we may have to resolve the same class more than once if it's referred
 * to from classes in multiple DEX files.  This is a necessary property for
 * DEX files associated with different class loaders.
 *
 * We cache a copy of the lookup in the DexFile's "resolved class" table,
 * so future references to "classIdx" are faster.
 *
 * Note that "referrer" may be in the process of being linked.
 *
 * Traditional VMs might do access checks here, but in Dalvik the class
 * "constant pool" is shared between all classes in the DEX file.  We rely
 * on the verifier to do the checks for us.
 *
 * Does not initialize the class.
 *
 * "fromUnverifiedConstant" should only be set if this call is the direct
 * result of executing a "const-class" or "instance-of" instruction, which
 * use class constants not resolved by the bytecode verifier.
 *
 * Returns NULL with an exception raised on failure.
 */
ClassObject* dvmResolveClass(const ClassObject* referrer, u4 classIdx,
    bool fromUnverifiedConstant)
{
    DvmDex* pDvmDex = referrer->pDvmDex;
    ClassObject* resClass;

    /*
     * Check the table first -- this gets called from the other "resolve"
     * methods.
     */
    resClass = dvmDexGetResolvedClass(pDvmDex, classIdx);
    if (resClass != NULL)
        return resClass;

    LOGVV("--- resolving class %u (referrer=%s cl=%p)\n",
        classIdx, referrer->descriptor, referrer->classLoader);

    /*
     * Class hasn't been loaded yet, or is in the process of being loaded
     * and initialized now.  Try to get a copy.  If we find one, put the
     * pointer in the DexTypeId.  There isn't a race condition here --
     * 32-bit writes are guaranteed atomic on all target platforms.  Worst
     * case we have two threads storing the same value.
     *
     * If this is an array class, we'll generate it here.
     */
    resClass = dvmFindClassNoInit(
            dexStringByTypeIdx(pDvmDex->pDexFile, classIdx),
            referrer->classLoader);

    if (resClass != NULL) {
        /*
         * If the referrer was pre-verified, the resolved class must come
         * from the same DEX or from a bootstrap class.  The pre-verifier
         * makes assumptions that could be invalidated by a wacky class
         * loader.  (See the notes at the top of oo/Class.c.)
         *
         * The verifier does *not* fail a class for using a const-class
         * or instance-of instruction referring to an unresolveable class,
         * because the result of the instruction is simply a Class object
         * or boolean -- there's no need to resolve the class object during
         * verification.  Instance field and virtual method accesses can
         * break dangerously if we get the wrong class, but const-class and
         * instance-of are only interesting at execution time.  So, if we
         * we got here as part of executing one of the "unverified class"
         * instructions, we skip the additional check.
         *
         * Ditto for class references from annotations and exception
         * handler lists.
         */
        if (!fromUnverifiedConstant &&
            IS_CLASS_FLAG_SET(referrer, CLASS_ISPREVERIFIED))
        {
            ClassObject* resClassCheck = resClass;
            if (dvmIsArrayClass(resClassCheck))
                resClassCheck = resClassCheck->elementClass;

            if (referrer->pDvmDex != resClassCheck->pDvmDex &&
                resClassCheck->classLoader != NULL)
            {
                LOGW("Class resolved by unexpected DEX:"
                     " %s(%p):%p ref [%s] %s(%p):%p\n",
                    referrer->descriptor, referrer->classLoader,
                    referrer->pDvmDex,
                    resClass->descriptor, resClassCheck->descriptor,
                    resClassCheck->classLoader, resClassCheck->pDvmDex);
                dvmThrowException("Ljava/lang/IllegalAccessError;",
                    "cross-loader access from pre-verified class");
                return NULL;
            }
        }

        LOGVV("##### +ResolveClass(%s): referrer=%s dex=%p ldr=%p ref=%d\n",
            resClass->descriptor, referrer->descriptor, referrer->pDvmDex,
            referrer->classLoader, classIdx);

        /*
         * Add what we found to the list so we can skip the class search
         * next time through.
         *
         * TODO: should we be doing this when fromUnverifiedConstant==true?
         * (see comments at top of oo/Class.c)
         */
        dvmDexSetResolvedClass(pDvmDex, classIdx, resClass);
    } else {
        /* not found, exception should be raised */
        LOGVV("Class not found: %s\n",
            dexStringByTypeIdx(pDvmDex->pDexFile, classIdx));
        assert(dvmCheckException(dvmThreadSelf()));
    }

    return resClass;
}


/*
 * Find the method corresponding to "methodRef".
 *
 * We use "referrer" to find the DexFile with the constant pool that
 * "methodRef" is an index into.  We also use its class loader.  The method
 * being resolved may very well be in a different DEX file.
 *
 * If this is a static method, we ensure that the method's class is
 * initialized.
 */
Method* dvmResolveMethod(const ClassObject* referrer, u4 methodIdx,
    MethodType methodType)
{
    DvmDex* pDvmDex = referrer->pDvmDex;
    ClassObject* resClass;
    const DexMethodId* pMethodId;
    Method* resMethod;

    assert(methodType != METHOD_INTERFACE);

    LOGVV("--- resolving method %u (referrer=%s)\n", methodIdx,
        referrer->descriptor);
    pMethodId = dexGetMethodId(pDvmDex->pDexFile, methodIdx);

    resClass = dvmResolveClass(referrer, pMethodId->classIdx, false);
    if (resClass == NULL) {
        /* can't find the class that the method is a part of */
        assert(dvmCheckException(dvmThreadSelf()));
        return NULL;
    }
    if (dvmIsInterfaceClass(resClass)) {
        /* method is part of an interface */
        dvmThrowExceptionWithClassMessage(
            "Ljava/lang/IncompatibleClassChangeError;",
            resClass->descriptor);
        return NULL;
    }

    const char* name = dexStringById(pDvmDex->pDexFile, pMethodId->nameIdx);
    DexProto proto;
    dexProtoSetFromMethodId(&proto, pDvmDex->pDexFile, pMethodId);

    /*
     * We need to chase up the class hierarchy to find methods defined
     * in super-classes.  (We only want to check the current class
     * if we're looking for a constructor; since DIRECT calls are only
     * for constructors and private methods, we don't want to walk up.)
     */
    if (methodType == METHOD_DIRECT) {
        resMethod = dvmFindDirectMethod(resClass, name, &proto);
    } else if (methodType == METHOD_STATIC) {
        resMethod = dvmFindDirectMethodHier(resClass, name, &proto);
    } else {
        resMethod = dvmFindVirtualMethodHier(resClass, name, &proto);
    }

    if (resMethod == NULL) {
        dvmThrowException("Ljava/lang/NoSuchMethodError;", name);
        return NULL;
    }

    LOGVV("--- found method %d (%s.%s)\n",
        methodIdx, resClass->descriptor, resMethod->name);

    /* see if this is a pure-abstract method */
    if (dvmIsAbstractMethod(resMethod) && !dvmIsAbstractClass(resClass)) {
        dvmThrowException("Ljava/lang/AbstractMethodError;", name);
        return NULL;
    }

    /*
     * If we're the first to resolve this class, we need to initialize
     * it now.  Only necessary for METHOD_STATIC.
     */
    if (methodType == METHOD_STATIC) {
        if (!dvmIsClassInitialized(resMethod->clazz) &&
            !dvmInitClass(resMethod->clazz))
        {
            assert(dvmCheckException(dvmThreadSelf()));
            return NULL;
        } else {
            assert(!dvmCheckException(dvmThreadSelf()));
        }
    } else {
        /*
         * Edge case: if the <clinit> for a class creates an instance
         * of itself, we will call <init> on a class that is still being
         * initialized by us.
         */
        assert(dvmIsClassInitialized(resMethod->clazz) ||
               dvmIsClassInitializing(resMethod->clazz));
    }

    /*
     * The class is initialized, the method has been found.  Add a pointer
     * to our data structure so we don't have to jump through the hoops again.
     */
    dvmDexSetResolvedMethod(pDvmDex, methodIdx, resMethod);

    return resMethod;
}

/*
 * Resolve an interface method reference.
 *
 * Returns NULL with an exception raised on failure.
 */
Method* dvmResolveInterfaceMethod(const ClassObject* referrer, u4 methodIdx)
{
    DvmDex* pDvmDex = referrer->pDvmDex;
    ClassObject* resClass;
    const DexMethodId* pMethodId;
    Method* resMethod;
    int i;

    LOGVV("--- resolving interface method %d (referrer=%s)\n",
        methodIdx, referrer->descriptor);
    pMethodId = dexGetMethodId(pDvmDex->pDexFile, methodIdx);

    resClass = dvmResolveClass(referrer, pMethodId->classIdx, false);
    if (resClass == NULL) {
        /* can't find the class that the method is a part of */
        assert(dvmCheckException(dvmThreadSelf()));
        return NULL;
    }
    if (!dvmIsInterfaceClass(resClass)) {
        /* whoops */
        dvmThrowExceptionWithClassMessage(
            "Ljava/lang/IncompatibleClassChangeError;",
            resClass->descriptor);
        return NULL;
    }

    /*
     * This is the first time the method has been resolved.  Set it in our
     * resolved-method structure.  It always resolves to the same thing,
     * so looking it up and storing it doesn't create a race condition.
     *
     * If we scan into the interface's superclass -- which is always
     * java/lang/Object -- we will catch things like:
     *   interface I ...
     *   I myobj = (something that implements I)
     *   myobj.hashCode()
     * However, the Method->methodIndex will be an offset into clazz->vtable,
     * rather than an offset into clazz->iftable.  The invoke-interface
     * code can test to see if the method returned is abstract or concrete,
     * and use methodIndex accordingly.  I'm not doing this yet because
     * (a) we waste time in an unusual case, and (b) we're probably going
     * to fix it in the DEX optimizer.
     *
     * We do need to scan the superinterfaces, in case we're invoking a
     * superinterface method on an interface reference.  The class in the
     * DexTypeId is for the static type of the object, not the class in
     * which the method is first defined.  We have the full, flattened
     * list in "iftable".
     */
    const char* methodName =
        dexStringById(pDvmDex->pDexFile, pMethodId->nameIdx);

    DexProto proto;
    dexProtoSetFromMethodId(&proto, pDvmDex->pDexFile, pMethodId);

    LOGVV("+++ looking for '%s' '%s' in resClass='%s'\n",
        methodName, methodSig, resClass->descriptor);
    resMethod = dvmFindVirtualMethod(resClass, methodName, &proto);
    if (resMethod == NULL) {
        LOGVV("+++ did not resolve immediately\n");
        for (i = 0; i < resClass->iftableCount; i++) {
            resMethod = dvmFindVirtualMethod(resClass->iftable[i].clazz,
                            methodName, &proto);
            if (resMethod != NULL)
                break;
        }

        if (resMethod == NULL) {
            dvmThrowException("Ljava/lang/NoSuchMethodError;", methodName);
            return NULL;
        }
    } else {
        LOGVV("+++ resolved immediately: %s (%s %d)\n", resMethod->name,
            resMethod->clazz->descriptor, (u4) resMethod->methodIndex);
    }

    LOGVV("--- found interface method %d (%s.%s)\n",
        methodIdx, resClass->descriptor, resMethod->name);

    /* we're expecting this to be abstract */
    assert(dvmIsAbstractMethod(resMethod));

    /* interface methods are always public; no need to check access */

    /*
     * The interface class *may* be initialized.  According to VM spec
     * v2 2.17.4, the interfaces a class refers to "need not" be initialized
     * when the class is initialized.
     *
     * It isn't necessary for an interface class to be initialized before
     * we resolve methods on that interface.
     *
     * We choose not to do the initialization now.
     */
    //assert(dvmIsClassInitialized(resMethod->clazz));

    /*
     * The class is initialized, the method has been found.  Add a pointer
     * to our data structure so we don't have to jump through the hoops again.
     */
    dvmDexSetResolvedMethod(pDvmDex, methodIdx, resMethod);

    return resMethod;
}

/*
 * Resolve an instance field reference.
 *
 * Returns NULL and throws an exception on error (no such field, illegal
 * access).
 */
InstField* dvmResolveInstField(const ClassObject* referrer, u4 ifieldIdx)
{
    DvmDex* pDvmDex = referrer->pDvmDex;
    ClassObject* resClass;
    const DexFieldId* pFieldId;
    InstField* resField;

    LOGVV("--- resolving field %u (referrer=%s cl=%p)\n",
        ifieldIdx, referrer->descriptor, referrer->classLoader);

    pFieldId = dexGetFieldId(pDvmDex->pDexFile, ifieldIdx);

    /*
     * Find the field's class.
     */
    resClass = dvmResolveClass(referrer, pFieldId->classIdx, false);
    if (resClass == NULL) {
        assert(dvmCheckException(dvmThreadSelf()));
        return NULL;
    }

    resField = dvmFindInstanceFieldHier(resClass,
        dexStringById(pDvmDex->pDexFile, pFieldId->nameIdx),
        dexStringByTypeIdx(pDvmDex->pDexFile, pFieldId->typeIdx));
    if (resField == NULL) {
        dvmThrowException("Ljava/lang/NoSuchFieldError;",
            dexStringById(pDvmDex->pDexFile, pFieldId->nameIdx));
        return NULL;
    }

    /*
     * Class must be initialized by now (unless verifier is buggy).  We
     * could still be in the process of initializing it if the field
     * access is from a static initializer.
     */
    assert(dvmIsClassInitialized(resField->field.clazz) ||
           dvmIsClassInitializing(resField->field.clazz));

    /*
     * The class is initialized, the method has been found.  Add a pointer
     * to our data structure so we don't have to jump through the hoops again.
     */
    dvmDexSetResolvedField(pDvmDex, ifieldIdx, (Field*)resField);
    LOGVV("    field %u is %s.%s\n",
        ifieldIdx, resField->field.clazz->descriptor, resField->field.name);

    return resField;
}

/*
 * Resolve a static field reference.  The DexFile format doesn't distinguish
 * between static and instance field references, so the "resolved" pointer
 * in the Dex struct will have the wrong type.  We trivially cast it here.
 *
 * Causes the field's class to be initialized.
 */
StaticField* dvmResolveStaticField(const ClassObject* referrer, u4 sfieldIdx)
{
    DvmDex* pDvmDex = referrer->pDvmDex;
    ClassObject* resClass;
    const DexFieldId* pFieldId;
    StaticField* resField;

    pFieldId = dexGetFieldId(pDvmDex->pDexFile, sfieldIdx);

    /*
     * Find the field's class.
     */
    resClass = dvmResolveClass(referrer, pFieldId->classIdx, false);
    if (resClass == NULL) {
        assert(dvmCheckException(dvmThreadSelf()));
        return NULL;
    }

    resField = dvmFindStaticFieldHier(resClass,
                dexStringById(pDvmDex->pDexFile, pFieldId->nameIdx),
                dexStringByTypeIdx(pDvmDex->pDexFile, pFieldId->typeIdx));
    if (resField == NULL) {
        dvmThrowException("Ljava/lang/NoSuchFieldError;",
            dexStringById(pDvmDex->pDexFile, pFieldId->nameIdx));
        return NULL;
    }

    /*
     * If we're the first to resolve the field in which this class resides,
     * we need to do it now.  Note that, if the field was inherited from
     * a superclass, it is not necessarily the same as "resClass".
     */
    if (!dvmIsClassInitialized(resField->field.clazz) &&
        !dvmInitClass(resField->field.clazz))
    {
        assert(dvmCheckException(dvmThreadSelf()));
        return NULL;
    }

    /*
     * The class is initialized, the method has been found.  Add a pointer
     * to our data structure so we don't have to jump through the hoops again.
     */
    dvmDexSetResolvedField(pDvmDex, sfieldIdx, (Field*) resField);

    return resField;
}


/*
 * Resolve a string reference.
 *
 * Finding the string is easy.  We need to return a reference to a
 * java/lang/String object, not a bunch of characters, which means the
 * first time we get here we need to create an interned string.
 */
StringObject* dvmResolveString(const ClassObject* referrer, u4 stringIdx)
{
    DvmDex* pDvmDex = referrer->pDvmDex;
    StringObject* strObj;
    StringObject* internStrObj;
    const char* utf8;
    u4 utf16Size;

    LOGVV("+++ resolving string, referrer is %s\n", referrer->descriptor);

    /*
     * Create a UTF-16 version so we can trivially compare it to what's
     * already interned.
     */
    utf8 = dexStringAndSizeById(pDvmDex->pDexFile, stringIdx, &utf16Size);
    strObj = dvmCreateStringFromCstrAndLength(utf8, utf16Size,
                ALLOC_DEFAULT);
    if (strObj == NULL) {
        /* ran out of space in GC heap? */
        assert(dvmCheckException(dvmThreadSelf()));
        goto bail;
    }

    /*
     * Add it to the intern list.  The return value is the one in the
     * intern list, which (due to race conditions) may or may not be
     * the one we just created.  The intern list is synchronized, so
     * there will be only one "live" version.
     *
     * By requesting an immortal interned string, we guarantee that
     * the returned object will never be collected by the GC.
     *
     * A NULL return here indicates some sort of hashing failure.
     */
    internStrObj = dvmLookupImmortalInternedString(strObj);
    dvmReleaseTrackedAlloc((Object*) strObj, NULL);
    strObj = internStrObj;
    if (strObj == NULL) {
        assert(dvmCheckException(dvmThreadSelf()));
        goto bail;
    }

    /* save a reference so we can go straight to the object next time */
    dvmDexSetResolvedString(pDvmDex, stringIdx, strObj);

bail:
    return strObj;
}

/*
 * For debugging: return a string representing the methodType.
 */
const char* dvmMethodTypeStr(MethodType methodType)
{
    switch (methodType) {
    case METHOD_DIRECT:         return "direct";
    case METHOD_STATIC:         return "static";
    case METHOD_VIRTUAL:        return "virtual";
    case METHOD_INTERFACE:      return "interface";
    case METHOD_UNKNOWN:        return "UNKNOWN";
    }
    assert(false);
    return "BOGUS";
}
