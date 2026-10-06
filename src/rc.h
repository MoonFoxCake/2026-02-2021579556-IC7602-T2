/*
 * rc.h - Códigos de retorno compartidos entre parser.c, ipcalc.c y routes.c
 *
 * Convención: toda función de ipcalc/routes devuelve RC_OK (0) si todo salió
 * bien, o un código negativo. El parser traduce cada código a un mensaje de
 * error para el usuario. Así nadie más imprime errores por su cuenta.
 */
#ifndef RC_H
#define RC_H

#define RC_OK             0
#define RC_ERR_IP        -1   /* dirección IP inválida                        */
#define RC_ERR_MASK      -2   /* máscara inválida                             */
#define RC_ERR_NOT_FOUND -3   /* DEL ROUTE de una ruta que no existe          */
#define RC_ERR_EXISTS    -4   /* SET ROUTE con un nombre ya existente         */
#define RC_ERR_RANGE     -5   /* SIZE/NUMBER incoherentes con la red base     */
#define RC_ERR_NO_ROUTE  -6   /* ROUTE IP: ninguna ruta coincide              */
#define RC_ERR_INTERNAL  -7   /* malloc falló, buffer muy chico, etc.         */
#define RC_ERR_ARG       -8   /* solo uso interno del parser                  */

#endif /* RC_H */
